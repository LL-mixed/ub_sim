#include "mem_service_daemon.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "mem_service_core.h"
#include "mem_service_object_refs.h"
#include "mem_service_record_table.h"
#include "mem_service_wire_client.h"
#include "mem_service_wire_payload.h"
#include "mem_service_wire_schema.h"

#define MEM_SERVICE_UNIX_SPEC_PREFIX "unix:"
#define MEM_SERVICE_TCP_SPEC_PREFIX "tcp:"
#define MEM_SERVICE_STORE_MAGIC "mem_service_store_v1"
#define MEM_SERVICE_JOURNAL_MAGIC "mem_service_journal_v1"
#define MEM_SERVICE_DURABLE_CATALOG_MAGIC "mem_service_durable_catalog_v1"
#define MEM_SERVICE_DURABLE_CATALOG_MANIFEST "manifest.txt"
#define MEM_SERVICE_SNAPSHOT_PAGE_HEADER_RESERVE 512U

static const uint64_t mem_service_latency_bucket_limits_ms
    [MEM_SERVICE_METRIC_LATENCY_BUCKET_COUNT] = {1U, 5U, 10U, 50U, 100U, UINT64_MAX};

static volatile sig_atomic_t mem_service_daemon_stop;
static uint64_t mem_service_payload_tmp_seq;

struct mem_service_restore_snapshot_stage {
    bool active;
    bool has_expected_records;
    bool saw_complete;
    uint64_t expected_records;
    uint64_t next_page_index;
    struct mem_service svc;
};

static struct mem_service_restore_snapshot_stage mem_service_restore_snapshot_stage;

static enum mem_service_wire_status mem_service_handle_operation(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    const char *store_path,
    const char *storage_root);
static enum mem_service_wire_status mem_service_dispatch_operation(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    const char *storage_root);
static enum mem_service_wire_status mem_service_metrics(struct mem_service *svc,
                                                        char *response,
                                                        size_t response_len);
static void mem_service_record_operation_metrics(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    enum mem_service_wire_status status,
    uint64_t latency_ms);
static bool mem_service_status_is_fail_closed(enum mem_service_wire_status status);
static bool mem_service_operation_mutates(enum mem_service_wire_operation operation,
                                          const char *payload);
static bool mem_service_payload_get_u64_checked(const char *payload,
                                                const char *name,
                                                uint64_t *out);
static uint64_t mem_service_audit_first_sequence(const struct mem_service *svc);
static const char *mem_service_record_kind_name(enum mem_service_record_kind kind);
static struct mem_service_idempotency_record *mem_service_find_idempotency_record(
    struct mem_service *svc,
    const char *key);
static struct mem_service_idempotency_record *mem_service_alloc_idempotency_record(
    struct mem_service *svc);

struct mem_service_store_import_state {
    struct mem_service_record record;
    struct mem_service_idempotency_record idempotency;
    struct mem_service_audit_event audit;
    bool in_record;
    bool in_idempotency;
    bool in_audit;
};

static int mem_service_expect_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual == expected) {
        return 0;
    }
    fprintf(stderr,
            "mem_service wire-fixtures: %s actual=%" PRIu64 " expected=%" PRIu64 "\n",
            name,
            actual,
            expected);
    return -1;
}

static int mem_service_expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 0;
    }
    fprintf(stderr,
            "mem_service wire-fixtures: %s actual=%u expected=%u\n",
            name,
            actual,
            expected);
    return -1;
}

struct mem_service_wire_payload_fixture {
    const char *name;
    enum mem_service_wire_operation operation;
    const char *payload;
    uint32_t expected_len;
    uint32_t expected_checksum;
};

static int mem_service_expect_payload_fixture(
    const struct mem_service_wire_payload_fixture *fixture)
{
    struct mem_service_wire_header header;
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(fixture->payload);
    const struct mem_service_wire_operation_schema *schema;
    const char *failed_field = NULL;
    uint32_t actual_len = (uint32_t)strlen(fixture->payload);
    uint32_t actual_checksum = mem_service_wire_checksum(fixture->payload, actual_len);
    int failures = 0;

    if (actual_len != fixture->expected_len) {
        fprintf(stderr,
                "mem_service wire-fixtures: %s len actual=%u expected=%u\n",
                fixture->name,
                actual_len,
                fixture->expected_len);
        failures -= 1;
    }
    if (actual_checksum != fixture->expected_checksum) {
        fprintf(stderr,
                "mem_service wire-fixtures: %s checksum actual=0x%08x expected=0x%08x\n",
                fixture->name,
                actual_checksum,
                fixture->expected_checksum);
        failures -= 1;
    }
    schema = mem_service_wire_schema_for_operation(fixture->operation);
    if (schema != NULL &&
        !mem_service_wire_schema_validate_payload(schema, &view, &failed_field)) {
        fprintf(stderr,
                "mem_service wire-fixtures: %s schema failed field=%s\n",
                fixture->name,
                failed_field != NULL ? failed_field : "unknown");
        failures -= 1;
    }

    mem_service_wire_init_header(&header,
                                 0,
                                 fixture->operation,
                                 actual_len,
                                 actual_checksum);
    failures += mem_service_expect_u32("fixture_header_operation",
                                       header.operation,
                                       fixture->operation);
    failures += mem_service_expect_u32("fixture_header_payload_len",
                                       header.payload_len,
                                       fixture->expected_len);
    failures += mem_service_expect_u32("fixture_header_payload_checksum",
                                       header.payload_checksum,
                                       fixture->expected_checksum);
    return failures;
}

struct mem_service_wire_response_fixture {
    const char *name;
    enum mem_service_wire_operation operation;
    const char *payload;
    enum mem_service_wire_status expected_status;
    uint32_t expected_len;
    uint32_t expected_checksum;
};

static int mem_service_expect_response_fixture(
    struct mem_service *svc,
    const struct mem_service_wire_response_fixture *fixture)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status;
    uint32_t actual_len;
    uint32_t actual_checksum;
    int failures = 0;

    memset(response, 0, sizeof(response));
    status = mem_service_dispatch_operation(svc,
                                            fixture->operation,
                                            fixture->payload,
                                            response,
                                            sizeof(response),
                                            NULL);
    mem_service_record_operation_metrics(svc, fixture->operation, status, 0);
    if (status != fixture->expected_status) {
        fprintf(stderr,
                "mem_service wire-fixtures: %s status actual=%s expected=%s\n",
                fixture->name,
                mem_service_wire_status_name(status),
                mem_service_wire_status_name(fixture->expected_status));
        failures -= 1;
    }
    actual_len = (uint32_t)strlen(response);
    actual_checksum = mem_service_wire_checksum(response, actual_len);
    if (actual_len != fixture->expected_len) {
        fprintf(stderr,
                "mem_service wire-fixtures: %s response_len actual=%u expected=%u\n",
                fixture->name,
                actual_len,
                fixture->expected_len);
        failures -= 1;
    }
    if (actual_checksum != fixture->expected_checksum) {
        fprintf(stderr,
                "mem_service wire-fixtures: %s response_checksum actual=0x%08x expected=0x%08x\n",
                fixture->name,
                actual_checksum,
                fixture->expected_checksum);
        failures -= 1;
    }
    return failures;
}

int mem_service_run_wire_fixture_check(void)
{
    static const struct mem_service_wire_payload_fixture fixtures[] = {
        {"health_request", MEM_SERVICE_WIRE_OP_HEALTH, "", 0, 0x00000000U},
        {"ready_request", MEM_SERVICE_WIRE_OP_READY, "", 0, 0x00000000U},
        {"status_request", MEM_SERVICE_WIRE_OP_STATUS, "", 0, 0x00000000U},
        {"list_records_request", MEM_SERVICE_WIRE_OP_LIST_RECORDS, "", 0, 0x00000000U},
        {"put_object_request",
         MEM_SERVICE_WIRE_OP_PUT_OBJECT,
         "key=fixture-object\n"
         "owner=2\n"
         "payload_kind=1\n"
         "backing_offset=64\n"
         "backing_len=128\n"
         "checksum=12345\n"
         "version=3\n",
         101,
         0x4e6f0ab1U},
        {"get_object_request",
         MEM_SERVICE_WIRE_OP_GET_OBJECT,
         "key=fixture-object\n",
         19,
         0x099d6fbeU},
        {"register_prefix_request",
         MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
         "request_id=req-a\n"
         "prefix_group=prefix-a\n"
         "group_id=group-a\n"
         "block_hash=block-a\n"
         "placement_node=1\n"
         "placement_level=2\n"
         "hot_segment_id=4096\n"
         "state=filled\n"
         "result_segment_id=8192\n",
         166,
         0x8a6bc143U},
        {"lookup_prefix_request",
         MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
         "request_id=req-a\n"
         "prefix_group=prefix-a\n",
         39,
         0x112e24c8U},
        {"publish_kv_request",
         MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
         "request_id=req-a\n"
         "prefix_group=prefix-a\n"
         "group_id=group-a\n"
         "block_hash=kv-block-a\n"
         "placement_node=1\n"
         "placement_level=2\n"
         "hot_segment_id=4096\n"
         "state=filled\n"
         "result_segment_id=12288\n",
         170,
         0xab96fa3cU},
        {"resolve_kv_request",
         MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
         "block_hash=kv-block-a\n",
         22,
         0x7b036ca5U},
        {"publish_runtime_handoff_request",
         MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
         "key=runtime/session-a/range-0\n"
         "session_id=session-a\n"
         "request_id=req-a\n"
         "model_key=model-a\n"
         "artifact_kind=hidden-range\n"
         "artifact_id=range-0\n"
         "owner=1\n"
         "payload_kind=2\n"
         "backing_offset=4096\n"
         "backing_len=8192\n"
         "checksum=1111\n"
         "version=7\n",
         217,
         0x9bdd9444U},
        {"resolve_runtime_handoff_request",
         MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
         "key=runtime/session-a/range-0\n"
         "expected_session_id=session-a\n"
         "expected_model_key=model-a\n"
         "expected_artifact_kind=hidden-range\n"
         "expected_artifact_id=range-0\n"
         "expected_version=7\n"
         "expected_checksum=1111\n",
         194,
         0x3e772698U},
        {"register_execution_artifact_request",
         MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
         "key=execution/session-a/logits-0\n"
         "session_id=session-a\n"
         "request_id=req-a\n"
         "model_key=model-a\n"
         "artifact_kind=logits\n"
         "artifact_id=logits-0\n"
         "owner=1\n"
         "payload_kind=3\n"
         "backing_offset=12288\n"
         "backing_len=4096\n"
         "checksum=2222\n"
         "version=8\n",
         216,
         0xe44a9059U},
        {"query_execution_artifact_request",
         MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
         "key=execution/session-a/logits-0\n"
         "expected_session_id=session-a\n"
         "expected_model_key=model-a\n"
         "expected_artifact_kind=logits\n"
         "expected_artifact_id=logits-0\n"
         "expected_version=8\n"
         "expected_checksum=2222\n",
         192,
         0xf601bf3fU},
        {"register_training_artifact_request",
         MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
         "key=training/run-a/checkpoint-0\n"
         "session_id=run-a\n"
         "model_key=model-a\n"
         "artifact_kind=checkpoint\n"
         "artifact_id=checkpoint-0\n"
         "owner=3\n"
         "payload_kind=4\n"
         "backing_offset=16384\n"
         "backing_len=32768\n"
         "checksum=3333\n"
         "version=9\n",
         203,
         0x70ed07a5U},
        {"query_training_artifact_request",
         MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
         "key=training/run-a/checkpoint-0\n"
         "expected_session_id=run-a\n"
         "expected_model_key=model-a\n"
         "expected_artifact_kind=checkpoint\n"
         "expected_artifact_id=checkpoint-0\n"
         "expected_version=9\n"
         "expected_checksum=3333\n",
         195,
         0x7ccb46a2U},
        {"metrics_request", MEM_SERVICE_WIRE_OP_METRICS, "", 0, 0x00000000U},
        {"export_snapshot_request", MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT, "", 0, 0x00000000U},
        {"export_snapshot_page_request",
         MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
         "start_index=0\n"
         "max_records=1\n",
         28,
         0x1b337d88U},
        {"inspect_object_request",
         MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
         "key=fixture-object\n",
         19,
         0x099d6fbeU},
        {"restore_snapshot_request",
         MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
         "mem_service_store_v1\n"
         "record_count=0\n",
         36,
         0x3fc9bd20U},
        {"restore_snapshot_page_request",
         MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
         "action=cancel\n",
         14,
         0xfe23a8a2U},
        {"audit_log_request", MEM_SERVICE_WIRE_OP_AUDIT_LOG, "", 0, 0x00000000U},
    };
    const struct mem_service_wire_response_fixture response_fixtures[] = {
        {"health_response",
         MEM_SERVICE_WIRE_OP_HEALTH,
         fixtures[0].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         2,
         0x663437afU},
        {"ready_response",
         MEM_SERVICE_WIRE_OP_READY,
         fixtures[1].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         2,
         0x663437afU},
        {"export_snapshot_response",
         MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
         fixtures[17].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         78,
         0x4c66d23cU},
        {"export_snapshot_page_response",
         MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
         fixtures[18].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         123,
         0xa5654285U},
        {"restore_snapshot_response",
         MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
         fixtures[20].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         36,
         0xdaa065aeU},
        {"restore_snapshot_page_response",
         MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
         fixtures[21].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         34,
         0xe54d9bffU},
        {"put_object_response",
         MEM_SERVICE_WIRE_OP_PUT_OBJECT,
         fixtures[4].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         39,
         0xa2b94d99U},
        {"get_object_response",
         MEM_SERVICE_WIRE_OP_GET_OBJECT,
         fixtures[5].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         343,
         0xe87e631eU},
        {"inspect_object_response",
         MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
         fixtures[19].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         383,
         0xabb21009U},
        {"register_prefix_response",
         MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
         fixtures[6].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         410,
         0x4f16a0c9U},
        {"lookup_prefix_response",
         MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
         fixtures[7].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         410,
         0x4f16a0c9U},
        {"publish_kv_response",
         MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
         fixtures[8].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         76,
         0x29d62b8bU},
        {"resolve_kv_response",
         MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
         fixtures[9].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         404,
         0x2c6ac21eU},
        {"publish_runtime_handoff_response",
         MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
         fixtures[10].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         423,
         0x6454ba82U},
        {"resolve_runtime_handoff_response",
         MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
         fixtures[11].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         423,
         0x6454ba82U},
        {"register_execution_artifact_response",
         MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
         fixtures[12].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         417,
         0x8d9812a7U},
        {"query_execution_artifact_response",
         MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
         fixtures[13].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         417,
         0x8d9812a7U},
        {"register_training_artifact_response",
         MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
         fixtures[14].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         424,
         0xde8843f2U},
        {"query_training_artifact_response",
         MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
         fixtures[15].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         424,
         0xde8843f2U},
        {"status_response",
         MEM_SERVICE_WIRE_OP_STATUS,
         fixtures[2].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         217,
         0x3f4609a1U},
        {"list_records_response",
         MEM_SERVICE_WIRE_OP_LIST_RECORDS,
         fixtures[3].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         784,
         0x3ae50a76U},
        {"metrics_response",
         MEM_SERVICE_WIRE_OP_METRICS,
         fixtures[16].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         1338,
         0x802c9350U},
        {"audit_log_response",
         MEM_SERVICE_WIRE_OP_AUDIT_LOG,
         fixtures[22].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         108,
         0xaac8ac2bU},
    };
    const struct mem_service_wire_payload_fixture *runtime_fixture = NULL;
    const struct mem_service_wire_payload_fixture *training_query_fixture = NULL;
    struct mem_service response_svc;
    struct mem_service idempotency_svc;
    struct mem_service_wire_header header;
    char idempotency_first[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char idempotency_replay[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char idempotency_conflict[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    size_t i;
    int failures = 0;
    static const char idempotency_payload[] =
        "key=runtime/fixture-idempotent/range-0\n"
        "session_id=session-idempotent\n"
        "model_key=model-idempotent\n"
        "artifact_kind=hidden-range\n"
        "artifact_id=range-0\n"
        "idempotency_key=fixture-idem-runtime\n";
    static const char idempotency_conflict_payload[] =
        "key=runtime/fixture-idempotent/range-0\n"
        "session_id=session-idempotent\n"
        "model_key=model-idempotent\n"
        "artifact_kind=hidden-range\n"
        "artifact_id=range-0\n"
        "checksum=777\n"
        "idempotency_key=fixture-idem-runtime\n";

    failures += mem_service_expect_u64(
        "header_size", sizeof(struct mem_service_wire_header), MEM_SERVICE_WIRE_HEADER_LEN);
    failures += mem_service_expect_u64(
        "offset_magic", offsetof(struct mem_service_wire_header, magic), 0);
    failures += mem_service_expect_u64(
        "offset_version", offsetof(struct mem_service_wire_header, version), 4);
    failures += mem_service_expect_u64(
        "offset_header_len", offsetof(struct mem_service_wire_header, header_len), 6);
    failures += mem_service_expect_u64(
        "offset_request_id", offsetof(struct mem_service_wire_header, request_id), 8);
    failures += mem_service_expect_u64(
        "offset_operation", offsetof(struct mem_service_wire_header, operation), 16);
    failures += mem_service_expect_u64(
        "offset_flags", offsetof(struct mem_service_wire_header, flags), 20);
    failures += mem_service_expect_u64(
        "offset_payload_len", offsetof(struct mem_service_wire_header, payload_len), 24);
    failures += mem_service_expect_u64("offset_payload_checksum",
                                       offsetof(struct mem_service_wire_header,
                                                payload_checksum),
                                       28);
    failures += mem_service_expect_u64(
        "offset_status", offsetof(struct mem_service_wire_header, status), 32);
    failures += mem_service_expect_u64(
        "offset_error_code", offsetof(struct mem_service_wire_header, error_code), 36);
    failures += mem_service_expect_u64("offset_server_time_ms",
                                       offsetof(struct mem_service_wire_header,
                                                server_time_ms),
                                       40);

    failures += mem_service_expect_u32("op_health", MEM_SERVICE_WIRE_OP_HEALTH, 1);
    failures += mem_service_expect_u32("op_ready", MEM_SERVICE_WIRE_OP_READY, 2);
    failures += mem_service_expect_u32("op_status", MEM_SERVICE_WIRE_OP_STATUS, 3);
    failures += mem_service_expect_u32("op_list_records", MEM_SERVICE_WIRE_OP_LIST_RECORDS, 4);
    failures += mem_service_expect_u32("op_metrics", MEM_SERVICE_WIRE_OP_METRICS, 5);
    failures += mem_service_expect_u32("op_export_snapshot",
                                       MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                       6);
    failures += mem_service_expect_u32("op_export_snapshot_page",
                                       MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                       7);
    failures += mem_service_expect_u32("op_restore_snapshot",
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                       8);
    failures += mem_service_expect_u32("op_restore_snapshot_page",
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       9);
    failures += mem_service_expect_u32("op_audit_log",
                                       MEM_SERVICE_WIRE_OP_AUDIT_LOG,
                                       10);
    failures += mem_service_expect_u32("op_put_object", MEM_SERVICE_WIRE_OP_PUT_OBJECT, 16);
    failures += mem_service_expect_u32("op_get_object", MEM_SERVICE_WIRE_OP_GET_OBJECT, 17);
    failures += mem_service_expect_u32("op_inspect_object",
                                       MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
                                       18);
    failures += mem_service_expect_u32("op_register_prefix",
                                       MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
                                       32);
    failures += mem_service_expect_u32("op_lookup_prefix",
                                       MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
                                       33);
    failures += mem_service_expect_u32("op_publish_kv",
                                       MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
                                       48);
    failures += mem_service_expect_u32("op_resolve_kv",
                                       MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
                                       49);
    failures += mem_service_expect_u32("op_publish_runtime",
                                       MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                       64);
    failures += mem_service_expect_u32("op_resolve_runtime",
                                       MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                       65);
    failures += mem_service_expect_u32("op_register_execution",
                                       MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
                                       80);
    failures += mem_service_expect_u32("op_query_execution",
                                       MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
                                       81);
    failures += mem_service_expect_u32("op_register_training",
                                       MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                       96);
    failures += mem_service_expect_u32("op_query_training",
                                       MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                       97);
    failures += mem_service_expect_u32("status_ok", MEM_SERVICE_WIRE_STATUS_OK, 0);
    failures += mem_service_expect_u32("status_stale",
                                       MEM_SERVICE_WIRE_STATUS_STALE_REF,
                                       2);
    failures += mem_service_expect_u32("status_checksum",
                                       MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH,
                                       3);
    failures += mem_service_expect_u32("status_unsupported",
                                       MEM_SERVICE_WIRE_STATUS_UNSUPPORTED,
                                       9);

    failures += mem_service_expect_u32("checksum_empty", mem_service_wire_checksum("", 0), 0);
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        failures += mem_service_expect_payload_fixture(&fixtures[i]);
        if (fixtures[i].operation == MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF) {
            runtime_fixture = &fixtures[i];
        }
        if (fixtures[i].operation == MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT) {
            training_query_fixture = &fixtures[i];
        }
    }
    if (runtime_fixture == NULL || training_query_fixture == NULL) {
        fprintf(stderr, "mem_service wire-fixtures: missing canonical payload fixture\n");
        failures -= 1;
    }
    if (mem_service_init(&response_svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service wire-fixtures: response service init failed\n");
        failures -= 1;
    } else {
        for (i = 0; i < sizeof(response_fixtures) / sizeof(response_fixtures[0]); ++i) {
            failures += mem_service_expect_response_fixture(&response_svc,
                                                            &response_fixtures[i]);
        }
    }
    if (mem_service_init(&idempotency_svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service wire-fixtures: idempotency service init failed\n");
        failures -= 1;
    } else {
        enum mem_service_wire_status first_status;
        enum mem_service_wire_status replay_status;
        enum mem_service_wire_status conflict_status;

        memset(idempotency_first, 0, sizeof(idempotency_first));
        memset(idempotency_replay, 0, sizeof(idempotency_replay));
        memset(idempotency_conflict, 0, sizeof(idempotency_conflict));
        first_status = mem_service_handle_operation(
            &idempotency_svc,
            MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
            idempotency_payload,
            idempotency_first,
            sizeof(idempotency_first),
            NULL,
            NULL);
        replay_status = mem_service_handle_operation(
            &idempotency_svc,
            MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
            idempotency_payload,
            idempotency_replay,
            sizeof(idempotency_replay),
            NULL,
            NULL);
        conflict_status = mem_service_handle_operation(
            &idempotency_svc,
            MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
            idempotency_conflict_payload,
            idempotency_conflict,
            sizeof(idempotency_conflict),
            NULL,
            NULL);
        if (first_status != MEM_SERVICE_WIRE_STATUS_OK ||
            replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
            conflict_status != MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT ||
            strcmp(idempotency_first, idempotency_replay) != 0 ||
            strstr(idempotency_replay, "version=1\n") == NULL ||
            strstr(idempotency_conflict, "idempotency_key=fixture-idem-runtime\n") == NULL ||
            idempotency_svc.metrics.idempotency_replay_count != 1U ||
            idempotency_svc.metrics.idempotency_conflict_count != 1U) {
            fprintf(stderr, "mem_service wire-fixtures: idempotency fixture failed\n");
            failures -= 1;
        }
    }
    if (failures != 0) {
        return 1;
    }

    mem_service_wire_init_header(&header,
                                 0x0102030405060708ULL,
                                 MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                 runtime_fixture->expected_len,
                                 runtime_fixture->expected_checksum);
    failures += mem_service_expect_u32("header_magic", header.magic, MEM_SERVICE_WIRE_MAGIC);
    failures += mem_service_expect_u32("header_version", header.version, MEM_SERVICE_WIRE_VERSION);
    failures += mem_service_expect_u32("header_len", header.header_len, MEM_SERVICE_WIRE_HEADER_LEN);
    failures += mem_service_expect_u64("header_request_id",
                                       header.request_id,
                                       0x0102030405060708ULL);
    failures += mem_service_expect_u32("header_operation",
                                       header.operation,
                                       MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF);
    failures += mem_service_expect_u32("header_payload_len",
                                       header.payload_len,
                                       runtime_fixture->expected_len);
    failures += mem_service_expect_u32("header_payload_checksum",
                                       header.payload_checksum,
                                       runtime_fixture->expected_checksum);
    failures += mem_service_expect_u32("header_status", header.status, MEM_SERVICE_WIRE_STATUS_OK);
    failures += mem_service_expect_u32(
        "header_error_code", header.error_code, MEM_SERVICE_WIRE_STATUS_OK);
    failures += mem_service_expect_u64("header_server_time_ms", header.server_time_ms, 0);

    if (failures != 0) {
        return 1;
    }
    printf("mem_service wire-fixtures: status=ok header_len=%u runtime_payload_len=%u "
           "runtime_checksum=0x%08x training_query_checksum=0x%08x "
           "payload_fixtures=%zu response_fixtures=%zu\n",
           MEM_SERVICE_WIRE_HEADER_LEN,
           runtime_fixture->expected_len,
           runtime_fixture->expected_checksum,
           training_query_fixture->expected_checksum,
           sizeof(fixtures) / sizeof(fixtures[0]),
           sizeof(response_fixtures) / sizeof(response_fixtures[0]));
    fflush(stdout);
    return 0;
}

static const char *mem_service_unix_path_from_spec(const char *spec)
{
    if (spec == NULL || spec[0] == '\0') {
        return NULL;
    }
    if (strncmp(spec, MEM_SERVICE_UNIX_SPEC_PREFIX,
                strlen(MEM_SERVICE_UNIX_SPEC_PREFIX)) == 0) {
        spec += strlen(MEM_SERVICE_UNIX_SPEC_PREFIX);
    }
    return spec[0] == '\0' ? NULL : spec;
}

static uint64_t mem_service_wall_clock_ms(void)
{
    time_t now = time(NULL);

    if (now < 0) {
        return 0;
    }
    return (uint64_t)now * 1000U;
}

static uint64_t mem_service_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return mem_service_wall_clock_ms();
    }
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void mem_service_signal_stop(int signo)
{
    (void)signo;
    mem_service_daemon_stop = 1;
}

static int mem_service_install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = mem_service_signal_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_read_full(int fd, void *buf, size_t len)
{
    uint8_t *cursor = (uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t rc = read(fd, cursor + done, len - done);

        if (rc == 0) {
            return -1;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

static int mem_service_write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *cursor = (const uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t rc = write(fd, cursor + done, len - done);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

static int mem_service_send_response(int fd,
                                     const struct mem_service_wire_header *request,
                                     enum mem_service_wire_status status,
                                     const char *payload)
{
    struct mem_service_wire_header response;
    uint32_t payload_len = 0;
    uint32_t checksum = 0;

    if (payload != NULL) {
        payload_len = (uint32_t)strlen(payload);
        checksum = mem_service_wire_checksum(payload, payload_len);
    }
    mem_service_wire_init_header(&response,
                                 request->request_id,
                                 (enum mem_service_wire_operation)request->operation,
                                 payload_len,
                                 checksum);
    response.status = (uint32_t)status;
    response.error_code = (uint32_t)status;
    response.server_time_ms = mem_service_wall_clock_ms();
    if (mem_service_write_full(fd, &response, sizeof(response)) != 0) {
        return -1;
    }
    if (payload_len > 0 &&
        mem_service_write_full(fd, payload, payload_len) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_read_payload(int fd,
                                    uint8_t *payload,
                                    uint32_t payload_len,
                                    uint32_t expected_checksum)
{
    if (payload_len > MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        return -1;
    }
    if (payload_len > 0 && mem_service_read_full(fd, payload, payload_len) != 0) {
        return -1;
    }
    payload[payload_len] = '\0';
    if (mem_service_wire_checksum(payload, payload_len) != expected_checksum) {
        return -1;
    }
    return 0;
}

static bool mem_service_payload_get_string(const char *payload,
                                           const char *name,
                                           char *out,
                                           size_t out_len)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);

    return mem_service_wire_payload_get_string(&view, name, out, out_len);
}

static bool mem_service_payload_get_header_string(const char *payload,
                                                  const char *name,
                                                  char *out,
                                                  size_t out_len)
{
    size_t name_len;
    const char *cursor;

    if (name == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (payload == NULL) {
        return false;
    }
    name_len = strlen(name);
    cursor = payload;
    while (cursor != NULL && *cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        const char *equals = memchr(cursor, '=', line_len);

        if (equals == NULL) {
            return false;
        }
        if ((size_t)(equals - cursor) == name_len &&
            strncmp(cursor, name, name_len) == 0) {
            size_t value_len = line_len - name_len - 1U;

            if (value_len >= out_len) {
                value_len = out_len - 1U;
            }
            memcpy(out, equals + 1, value_len);
            out[value_len] = '\0';
            return out[0] != '\0';
        }
        cursor = line_end ? line_end + 1 : NULL;
    }
    return false;
}

static uint64_t mem_service_payload_get_u64(const char *payload,
                                            const char *name,
                                            uint64_t default_value)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);

    return mem_service_wire_payload_get_u64(&view, name, default_value);
}

static uint32_t mem_service_payload_get_u32(const char *payload,
                                            const char *name,
                                            uint32_t default_value)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);

    return mem_service_wire_payload_get_u32(&view, name, default_value);
}

static void mem_service_trim_line(char *line)
{
    size_t len;

    if (line == NULL) {
        return;
    }
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len -= 1;
    }
}

static bool mem_service_parse_line_field(const char *line,
                                         char *name,
                                         size_t name_len,
                                         char *value,
                                         size_t value_len)
{
    const char *equals;
    size_t field_len;
    size_t copy_len;

    if (line == NULL || name == NULL || value == NULL || name_len == 0 ||
        value_len == 0) {
        return false;
    }
    equals = strchr(line, '=');
    if (equals == NULL || equals == line) {
        return false;
    }
    field_len = (size_t)(equals - line);
    if (field_len >= name_len) {
        field_len = name_len - 1;
    }
    memcpy(name, line, field_len);
    name[field_len] = '\0';

    copy_len = strlen(equals + 1);
    if (copy_len >= value_len) {
        copy_len = value_len - 1;
    }
    memcpy(value, equals + 1, copy_len);
    value[copy_len] = '\0';
    return true;
}

static uint64_t mem_service_parse_u64_value(const char *value, uint64_t default_value)
{
    char *end = NULL;
    uint64_t parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return default_value;
    }
    return parsed;
}

static int64_t mem_service_parse_i64_value(const char *value, int64_t default_value)
{
    char *end = NULL;
    int64_t parsed;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    errno = 0;
    parsed = strtoll(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return default_value;
    }
    return parsed;
}

static uint32_t mem_service_parse_u32_value(const char *value, uint32_t default_value)
{
    uint64_t parsed = mem_service_parse_u64_value(value, default_value);

    if (parsed > UINT32_MAX) {
        return default_value;
    }
    return (uint32_t)parsed;
}

static int mem_service_member_index_from_name(const char *name)
{
    char *end = NULL;
    unsigned long index;

    if (name == NULL || strncmp(name, "member", 6) != 0 || name[6] == '\0') {
        return -1;
    }
    errno = 0;
    index = strtoul(name + 6, &end, 10);
    if (errno != 0 || end == name + 6 || *end != '\0' ||
        index >= MEM_SERVICE_MAX_GROUP_MEMBERS) {
        return -1;
    }
    return (int)index;
}

static void mem_service_copy_store_string(char *out, size_t out_len, const char *value)
{
    size_t copy_len;

    if (out_len == 0) {
        return;
    }
    if (value == NULL) {
        out[0] = '\0';
        return;
    }
    copy_len = strlen(value);
    if (copy_len >= out_len) {
        copy_len = out_len - 1;
    }
    memcpy(out, value, copy_len);
    out[copy_len] = '\0';
}

static int mem_service_parse_store_record_field(struct mem_service_record *record,
                                                const char *name,
                                                const char *value)
{
    int member_index = mem_service_member_index_from_name(name);

    if (member_index >= 0) {
        mem_service_copy_store_string(record->member_block_hashes[member_index],
                                      sizeof(record->member_block_hashes[member_index]),
                                      value);
        if (record->member_count <= (uint32_t)member_index) {
            record->member_count = (uint32_t)member_index + 1U;
        }
        return 0;
    }
    if (strcmp(name, "kind") == 0) {
        record->kind = (enum mem_service_record_kind)mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "key") == 0) {
        mem_service_copy_store_string(record->key, sizeof(record->key), value);
    } else if (strcmp(name, "request_id") == 0) {
        mem_service_copy_store_string(record->request_id, sizeof(record->request_id), value);
    } else if (strcmp(name, "prefix_group") == 0) {
        mem_service_copy_store_string(record->prefix_group, sizeof(record->prefix_group), value);
    } else if (strcmp(name, "group_id") == 0) {
        mem_service_copy_store_string(record->group_id, sizeof(record->group_id), value);
    } else if (strcmp(name, "block_hash") == 0) {
        mem_service_copy_store_string(record->block_hash, sizeof(record->block_hash), value);
    } else if (strcmp(name, "session_id") == 0) {
        mem_service_copy_store_string(record->session_id, sizeof(record->session_id), value);
    } else if (strcmp(name, "model_key") == 0) {
        mem_service_copy_store_string(record->model_key, sizeof(record->model_key), value);
    } else if (strcmp(name, "artifact_kind") == 0) {
        mem_service_copy_store_string(record->artifact_kind,
                                      sizeof(record->artifact_kind),
                                      value);
    } else if (strcmp(name, "artifact_id") == 0) {
        mem_service_copy_store_string(record->artifact_id, sizeof(record->artifact_id), value);
    } else if (strcmp(name, "placement_node") == 0) {
        record->placement_node = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "placement_level") == 0) {
        record->placement_level = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "hot_segment_id") == 0) {
        record->hot_segment_id = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "state") == 0) {
        record->state = (enum mem_service_kvcache_state)mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "version") == 0) {
        record->version = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "last_result_segment") == 0) {
        record->last_result_segment = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_owner_node") == 0) {
        record->object_owner_node = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "object_payload_kind") == 0) {
        record->object_payload_kind = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "object_backing_offset") == 0) {
        record->object_backing_offset = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_backing_len") == 0) {
        record->object_backing_len = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_payload_checksum") == 0) {
        record->object_payload_checksum = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_publish_monotonic_ms") == 0) {
        record->object_publish_monotonic_ms = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_publish_supernode_ms") == 0) {
        record->object_publish_supernode_ms = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_publish_supernode_offset_ms") == 0) {
        record->object_publish_supernode_offset_ms =
            mem_service_parse_i64_value(value, 0);
    } else if (strcmp(name, "member_count") == 0) {
        uint32_t count = mem_service_parse_u32_value(value, 0);

        record->member_count = count > MEM_SERVICE_MAX_GROUP_MEMBERS
                                   ? MEM_SERVICE_MAX_GROUP_MEMBERS
                                   : count;
    }
    return 0;
}

static int mem_service_store_import_record(struct mem_service *svc,
                                           const struct mem_service_record *record)
{
    struct mem_service_record *slot;

    if (record->key[0] == '\0' || record->kind == 0) {
        return -1;
    }
    slot = mem_service_find_record(svc, record->key);
    if (slot == NULL) {
        slot = mem_service_alloc_record(svc);
    }
    if (slot == NULL) {
        return -1;
    }
    *slot = *record;
    slot->in_use = true;
    return 0;
}

static int mem_service_append_idempotency_response_line(
    struct mem_service_idempotency_record *record,
    const char *line)
{
    size_t used;
    size_t line_len;

    if (record == NULL || line == NULL) {
        return -1;
    }
    used = record->response_len;
    line_len = strlen(line);
    if (used + line_len + 1U >= sizeof(record->response)) {
        return -1;
    }
    memcpy(record->response + used, line, line_len);
    used += line_len;
    record->response[used] = '\n';
    used += 1U;
    record->response[used] = '\0';
    record->response_len = (uint32_t)used;
    return 0;
}

static int mem_service_parse_store_idempotency_field(
    struct mem_service_idempotency_record *record,
    const char *name,
    const char *value)
{
    if (strcmp(name, "key") == 0) {
        mem_service_copy_store_string(record->key, sizeof(record->key), value);
    } else if (strcmp(name, "operation") == 0) {
        record->operation = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "request_checksum") == 0) {
        record->request_checksum = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "status") == 0) {
        record->status = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "response_len") == 0) {
        (void)value;
    } else if (strcmp(name, "response_line") == 0) {
        return mem_service_append_idempotency_response_line(record, value);
    }
    return 0;
}

static int mem_service_store_import_idempotency(
    struct mem_service *svc,
    const struct mem_service_idempotency_record *record)
{
    struct mem_service_idempotency_record *slot;

    if (record->key[0] == '\0' || record->operation == 0) {
        return -1;
    }
    slot = mem_service_find_idempotency_record(svc, record->key);
    if (slot == NULL) {
        slot = mem_service_alloc_idempotency_record(svc);
    }
    if (slot == NULL) {
        return -1;
    }
    *slot = *record;
    slot->in_use = true;
    if (slot->response_len >= sizeof(slot->response)) {
        slot->response_len = sizeof(slot->response) - 1U;
    }
    slot->response[slot->response_len] = '\0';
    return 0;
}

static int mem_service_parse_store_audit_field(struct mem_service_audit_event *event,
                                               const char *name,
                                               const char *value)
{
    if (strcmp(name, "sequence") == 0) {
        event->sequence = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "monotonic_ms") == 0) {
        event->monotonic_ms = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "operation") == 0) {
        event->operation = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "status") == 0) {
        event->status = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "request_checksum") == 0) {
        event->request_checksum = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "response_checksum") == 0) {
        event->response_checksum = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "idempotency_replay") == 0) {
        event->idempotency_replay = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "key") == 0) {
        mem_service_copy_store_string(event->key, sizeof(event->key), value);
    } else if (strcmp(name, "session_id") == 0) {
        mem_service_copy_store_string(event->session_id,
                                      sizeof(event->session_id),
                                      value);
    } else if (strcmp(name, "model_key") == 0) {
        mem_service_copy_store_string(event->model_key, sizeof(event->model_key), value);
    } else if (strcmp(name, "artifact_kind") == 0) {
        mem_service_copy_store_string(event->artifact_kind,
                                      sizeof(event->artifact_kind),
                                      value);
    } else if (strcmp(name, "artifact_id") == 0) {
        mem_service_copy_store_string(event->artifact_id,
                                      sizeof(event->artifact_id),
                                      value);
    } else if (strcmp(name, "idempotency_key") == 0) {
        mem_service_copy_store_string(event->idempotency_key,
                                      sizeof(event->idempotency_key),
                                      value);
    } else if (strcmp(name, "version") == 0) {
        event->version = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "checksum") == 0) {
        event->checksum = mem_service_parse_u64_value(value, 0);
    }
    return 0;
}

static int mem_service_store_import_audit(struct mem_service *svc,
                                          const struct mem_service_audit_event *event)
{
    size_t slot_index;
    bool duplicate_sequence = false;

    if (svc == NULL || event == NULL || event->sequence == 0 ||
        event->operation == 0) {
        return -1;
    }
    slot_index = (size_t)((event->sequence - 1U) % MEM_SERVICE_MAX_AUDIT_EVENTS);
    duplicate_sequence = svc->audit_events[slot_index].in_use &&
                         svc->audit_events[slot_index].sequence == event->sequence;
    svc->audit_events[slot_index] = *event;
    svc->audit_events[slot_index].in_use = true;
    if (event->sequence >= svc->audit_next_sequence) {
        svc->audit_next_sequence = event->sequence + 1U;
    }
    if (!duplicate_sequence &&
        svc->audit_event_count < MEM_SERVICE_MAX_AUDIT_EVENTS) {
        svc->audit_event_count += 1U;
    }
    return 0;
}

static int mem_service_import_store_line(struct mem_service *svc,
                                         const char *line,
                                         struct mem_service_store_import_state *state)
{
    char name[96];
    char value[512];

    if (line == NULL || state == NULL) {
        return -1;
    }
    if (line[0] == '\0') {
        return 0;
    }
    if (strcmp(line, "record_begin") == 0) {
        if (state->in_record || state->in_idempotency || state->in_audit) {
            return -1;
        }
        memset(&state->record, 0, sizeof(state->record));
        state->record.in_use = true;
        state->in_record = true;
        return 0;
    }
    if (strcmp(line, "record_end") == 0) {
        if (!state->in_record ||
            mem_service_store_import_record(svc, &state->record) != 0) {
            return -1;
        }
        memset(&state->record, 0, sizeof(state->record));
        state->in_record = false;
        return 0;
    }
    if (strcmp(line, "idempotency_begin") == 0) {
        if (state->in_record || state->in_idempotency || state->in_audit) {
            return -1;
        }
        memset(&state->idempotency, 0, sizeof(state->idempotency));
        state->idempotency.in_use = true;
        state->in_idempotency = true;
        return 0;
    }
    if (strcmp(line, "idempotency_end") == 0) {
        if (!state->in_idempotency ||
            mem_service_store_import_idempotency(svc, &state->idempotency) != 0) {
            return -1;
        }
        memset(&state->idempotency, 0, sizeof(state->idempotency));
        state->in_idempotency = false;
        return 0;
    }
    if (strcmp(line, "audit_begin") == 0) {
        if (state->in_record || state->in_idempotency || state->in_audit) {
            return -1;
        }
        memset(&state->audit, 0, sizeof(state->audit));
        state->audit.in_use = true;
        state->in_audit = true;
        return 0;
    }
    if (strcmp(line, "audit_end") == 0) {
        if (!state->in_audit ||
            mem_service_store_import_audit(svc, &state->audit) != 0) {
            return -1;
        }
        memset(&state->audit, 0, sizeof(state->audit));
        state->in_audit = false;
        return 0;
    }
    if (!mem_service_parse_line_field(line, name, sizeof(name), value, sizeof(value))) {
        return -1;
    }
    if (state->in_record) {
        return mem_service_parse_store_record_field(&state->record, name, value);
    }
    if (state->in_idempotency) {
        return mem_service_parse_store_idempotency_field(&state->idempotency, name, value);
    }
    if (state->in_audit) {
        return mem_service_parse_store_audit_field(&state->audit, name, value);
    }
    return 0;
}

static int mem_service_import_snapshot_text(struct mem_service *svc,
                                            const char *snapshot)
{
    const char *cursor = snapshot;
    struct mem_service_store_import_state state;
    bool saw_magic = false;

    if (svc == NULL || snapshot == NULL || snapshot[0] == '\0') {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    while (cursor[0] != '\0') {
        const char *newline = strchr(cursor, '\n');
        char line[512];
        size_t line_len = newline != NULL ? (size_t)(newline - cursor) : strlen(cursor);

        if (line_len >= sizeof(line)) {
            return -1;
        }
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';
        mem_service_trim_line(line);
        if (!saw_magic) {
            if (strcmp(line, MEM_SERVICE_STORE_MAGIC) != 0) {
                return -1;
            }
            saw_magic = true;
        } else if (mem_service_import_store_line(svc, line, &state) != 0) {
            return -1;
        }
        if (newline == NULL) {
            break;
        }
        cursor = newline + 1;
    }
    return saw_magic && !state.in_record && !state.in_idempotency &&
                   !state.in_audit
               ? 0
               : -1;
}

static int mem_service_import_snapshot_records_text(struct mem_service *svc,
                                                    const char *snapshot,
                                                    size_t *records_imported_out)
{
    const char *cursor = snapshot;
    struct mem_service_store_import_state state;
    size_t records_imported = 0;

    if (svc == NULL || snapshot == NULL || snapshot[0] == '\0') {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    while (cursor[0] != '\0') {
        const char *newline = strchr(cursor, '\n');
        char line[512];
        size_t line_len = newline != NULL ? (size_t)(newline - cursor) : strlen(cursor);
        bool is_record_end;

        if (line_len >= sizeof(line)) {
            return -1;
        }
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';
        mem_service_trim_line(line);
        is_record_end = strcmp(line, "record_end") == 0;
        if (line[0] != '\0' &&
            mem_service_import_store_line(svc, line, &state) != 0) {
            return -1;
        }
        if (is_record_end) {
            records_imported += 1U;
        }
        if (newline == NULL) {
            break;
        }
        cursor = newline + 1;
    }
    if (state.in_record || state.in_idempotency || state.in_audit) {
        return -1;
    }
    if (records_imported_out != NULL) {
        *records_imported_out = records_imported;
    }
    return 0;
}

static int mem_service_make_journal_path(const char *store_path,
                                         char *journal_path,
                                         size_t journal_path_len)
{
    if (store_path == NULL || store_path[0] == '\0' ||
        journal_path == NULL || journal_path_len == 0) {
        return -1;
    }
    return snprintf(journal_path,
                    journal_path_len,
                    "%s.journal",
                    store_path) < (int)journal_path_len
               ? 0
               : -1;
}

static int mem_service_join_path(char *out,
                                 size_t out_len,
                                 const char *base,
                                 const char *name)
{
    size_t base_len;
    const char *separator = "/";

    if (out == NULL || out_len == 0 || base == NULL || base[0] == '\0' ||
        name == NULL || name[0] == '\0') {
        return -1;
    }
    base_len = strlen(base);
    if (base_len > 0 && base[base_len - 1U] == '/') {
        separator = "";
    }
    return snprintf(out, out_len, "%s%s%s", base, separator, name) < (int)out_len
               ? 0
               : -1;
}

static int mem_service_ensure_dir(const char *path)
{
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }
    if (stat(path, &st) != 0) {
        return -1;
    }
    return S_ISDIR(st.st_mode) ? 0 : -1;
}

static bool mem_service_path_is_dir(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mem_service_make_catalog_path(const char *storage_root,
                                         const char *name,
                                         char *path,
                                         size_t path_len)
{
    char catalog_dir[512];

    if (mem_service_join_path(catalog_dir,
                              sizeof(catalog_dir),
                              storage_root,
                              "catalog") != 0) {
        return -1;
    }
    return mem_service_join_path(path, path_len, catalog_dir, name);
}

static int mem_service_prepare_durable_catalog_layout(const char *storage_root)
{
    char catalog_dir[512];
    char block_dir[512];
    char quarantine_dir[512];

    if (storage_root == NULL || storage_root[0] == '\0') {
        return 0;
    }
    if (mem_service_join_path(catalog_dir,
                              sizeof(catalog_dir),
                              storage_root,
                              "catalog") != 0 ||
        mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0) {
        return -1;
    }
    if (mem_service_ensure_dir(storage_root) != 0 ||
        mem_service_ensure_dir(catalog_dir) != 0 ||
        mem_service_ensure_dir(block_dir) != 0 ||
        mem_service_ensure_dir(quarantine_dir) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_write_durable_catalog_manifest(const char *storage_root,
                                                      const char *store_path)
{
    char manifest_path[512];
    char catalog_dir[512];
    char block_dir[512];
    char quarantine_dir[512];
    char journal_path[512];
    char tmp_path[512];
    FILE *file;
    int write_rc;

    if (storage_root == NULL || storage_root[0] == '\0') {
        return 0;
    }
    if (store_path == NULL || store_path[0] == '\0') {
        return -1;
    }
    if (mem_service_make_catalog_path(storage_root,
                                      MEM_SERVICE_DURABLE_CATALOG_MANIFEST,
                                      manifest_path,
                                      sizeof(manifest_path)) != 0 ||
        mem_service_join_path(catalog_dir,
                              sizeof(catalog_dir),
                              storage_root,
                              "catalog") != 0 ||
        mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0 ||
        snprintf(tmp_path,
                 sizeof(tmp_path),
                 "%s.tmp.%ld",
                 manifest_path,
                 (long)getpid()) >= (int)sizeof(tmp_path)) {
        return -1;
    }
    file = fopen(tmp_path, "w");
    if (file == NULL) {
        return -1;
    }
    write_rc = fprintf(file,
                       "%s\n"
                       "layout=storage-root-v1\n"
                       "catalog_dir=%s\n"
                       "block_dir=%s\n"
                       "quarantine_dir=%s\n"
                       "store_path=%s\n"
                       "journal_path=%s\n"
                       "store_magic=%s\n"
                       "journal_magic=%s\n"
                       "payload_block_backend=sealed-local-block-v1\n"
                       "corrupt_payload_policy=quarantine-fail-closed\n",
                       MEM_SERVICE_DURABLE_CATALOG_MAGIC,
                       catalog_dir,
                       block_dir,
                       quarantine_dir,
                       store_path,
                       journal_path,
                       MEM_SERVICE_STORE_MAGIC,
                       MEM_SERVICE_JOURNAL_MAGIC);
    if (fclose(file) != 0 || write_rc < 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, manifest_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int mem_service_make_payload_block_path(const char *storage_root,
                                               uint64_t checksum,
                                               char *path,
                                               size_t path_len)
{
    char block_dir[512];
    char block_name[48];

    if (mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "blocks") != 0 ||
        snprintf(block_name,
                 sizeof(block_name),
                 "%016" PRIx64 ".block",
                 checksum) >= (int)sizeof(block_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, block_dir, block_name);
}

static int mem_service_make_payload_quarantine_path(const char *storage_root,
                                                    uint64_t checksum,
                                                    char *path,
                                                    size_t path_len)
{
    char quarantine_dir[512];
    char block_name[64];

    if (mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        snprintf(block_name,
                 sizeof(block_name),
                 "%016" PRIx64 ".bad.%ld",
                 checksum,
                 (long)getpid()) >= (int)sizeof(block_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, quarantine_dir, block_name);
}

static void mem_service_quarantine_payload_block(const char *storage_root,
                                                 uint64_t checksum)
{
    char block_path[512];
    char quarantine_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        mem_service_make_payload_block_path(storage_root,
                                            checksum,
                                            block_path,
                                            sizeof(block_path)) != 0 ||
        mem_service_make_payload_quarantine_path(storage_root,
                                                checksum,
                                                quarantine_path,
                                                sizeof(quarantine_path)) != 0) {
        return;
    }
    (void)rename(block_path, quarantine_path);
}

static int mem_service_make_payload_tmp_path(const char *storage_root,
                                             char *path,
                                             size_t path_len)
{
    char block_dir[512];
    char tmp_name[64];
    uint64_t seq = ++mem_service_payload_tmp_seq;

    if (mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "blocks") != 0 ||
        snprintf(tmp_name,
                 sizeof(tmp_name),
                 "ingest.tmp.%ld.%" PRIu64 ".%llu",
                 (long)getpid(),
                 seq,
                 (unsigned long long)mem_service_wall_clock_ms()) >=
            (int)sizeof(tmp_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, block_dir, tmp_name);
}

static enum mem_service_wire_status mem_service_copy_payload_file_to_tmp(
    const char *payload_path,
    const char *tmp_path,
    uint64_t *actual_len_out,
    uint64_t *actual_checksum_out)
{
    FILE *src;
    FILE *dst;
    uint8_t buffer[4096];
    uint64_t actual_len = 0;
    uint64_t hash = 1469598103934665603ULL;

    if (payload_path == NULL || payload_path[0] == '\0' ||
        tmp_path == NULL || tmp_path[0] == '\0' ||
        actual_len_out == NULL || actual_checksum_out == NULL) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    src = fopen(payload_path, "rb");
    if (src == NULL) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    dst = fopen(tmp_path, "wb");
    if (dst == NULL) {
        fclose(src);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    for (;;) {
        size_t got = fread(buffer, 1U, sizeof(buffer), src);
        size_t i;

        if (got > 0U &&
            fwrite(buffer, 1U, got, dst) != got) {
            fclose(src);
            fclose(dst);
            unlink(tmp_path);
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        for (i = 0; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
        actual_len += (uint64_t)got;
        if (got < sizeof(buffer)) {
            if (ferror(src)) {
                fclose(src);
                fclose(dst);
                unlink(tmp_path);
                return MEM_SERVICE_WIRE_STATUS_INTERNAL;
            }
            break;
        }
    }
    if (fclose(src) != 0) {
        fclose(dst);
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (fclose(dst) != 0) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    *actual_len_out = actual_len;
    *actual_checksum_out = hash;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_write_payload_block(
    const char *storage_root,
    const char *payload,
    const char *payload_inline,
    const char *payload_path,
    struct mem_service_record *record)
{
    char block_path[512];
    char tmp_path[512];
    uint64_t expected_len = 0;
    uint64_t expected_checksum = 0;
    uint64_t actual_len = 0;
    uint64_t actual_checksum = 0;
    bool has_inline = payload_inline != NULL && payload_inline[0] != '\0';
    bool has_path = payload_path != NULL && payload_path[0] != '\0';
    FILE *file;
    enum mem_service_wire_status status;

    if (!has_inline && !has_path) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (has_inline && has_path) {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (record == NULL || storage_root == NULL || storage_root[0] == '\0') {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (mem_service_payload_get_u32(payload, "payload_kind", 0) != 0 &&
        mem_service_payload_get_u32(payload, "payload_kind", 0) !=
            MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK) {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (has_inline) {
        actual_len = (uint64_t)strlen(payload_inline);
        actual_checksum =
            mem_service_checksum_bytes((const uint8_t *)payload_inline,
                                       actual_len);
    } else {
        if (mem_service_make_payload_tmp_path(storage_root,
                                              tmp_path,
                                              sizeof(tmp_path)) != 0) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        status = mem_service_copy_payload_file_to_tmp(payload_path,
                                                      tmp_path,
                                                      &actual_len,
                                                      &actual_checksum);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            return status;
        }
    }
    if (mem_service_payload_get_u64_checked(payload, "backing_len", &expected_len) &&
        expected_len != actual_len) {
        if (has_path) {
            unlink(tmp_path);
        }
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_payload_get_u64_checked(payload, "checksum", &expected_checksum) &&
        expected_checksum != actual_checksum) {
        if (has_path) {
            unlink(tmp_path);
        }
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_make_payload_block_path(storage_root,
                                            actual_checksum,
                                            block_path,
                                            sizeof(block_path)) != 0) {
        if (has_path) {
            unlink(tmp_path);
        }
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (has_inline) {
        if (mem_service_make_payload_tmp_path(storage_root,
                                              tmp_path,
                                              sizeof(tmp_path)) != 0) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        file = fopen(tmp_path, "wb");
        if (file == NULL) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        if (actual_len > 0U &&
            fwrite(payload_inline, 1U, (size_t)actual_len, file) !=
                (size_t)actual_len) {
            fclose(file);
            unlink(tmp_path);
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        if (fclose(file) != 0) {
            unlink(tmp_path);
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
    }
    if (rename(tmp_path, block_path) != 0) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK;
    record->object_backing_offset = 0;
    record->object_backing_len = actual_len;
    record->object_payload_checksum = actual_checksum;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_validate_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char block_path[512];
    uint8_t buffer[1024];
    uint64_t hash = 1469598103934665603ULL;
    uint64_t actual_len = 0;
    FILE *file;

    if (record == NULL ||
        record->object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (storage_root == NULL || storage_root[0] == '\0' ||
        record->object_payload_checksum == 0U ||
        mem_service_make_payload_block_path(storage_root,
                                            record->object_payload_checksum,
                                            block_path,
                                            sizeof(block_path)) != 0) {
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    file = fopen(block_path, "rb");
    if (file == NULL) {
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    for (;;) {
        size_t got = fread(buffer, 1U, sizeof(buffer), file);
        size_t i;

        for (i = 0; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
        actual_len += (uint64_t)got;
        if (got < sizeof(buffer)) {
            if (ferror(file)) {
                fclose(file);
                mem_service_quarantine_payload_block(storage_root,
                                                     record->object_payload_checksum);
                return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
            }
            break;
        }
    }
    fclose(file);
    if (actual_len != record->object_backing_len ||
        hash != record->object_payload_checksum) {
        mem_service_quarantine_payload_block(storage_root,
                                             record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static int mem_service_load_store(struct mem_service *svc, const char *store_path)
{
    FILE *file;
    char line[512];
    struct mem_service_store_import_state state;

    if (store_path == NULL || store_path[0] == '\0') {
        return 0;
    }
    file = fopen(store_path, "r");
    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    mem_service_trim_line(line);
    if (strcmp(line, MEM_SERVICE_STORE_MAGIC) != 0) {
        fclose(file);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    while (fgets(line, sizeof(line), file) != NULL) {
        mem_service_trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (mem_service_import_store_line(svc, line, &state) != 0) {
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    return state.in_record || state.in_idempotency || state.in_audit ? -1 : 0;
}

static int mem_service_load_journal(struct mem_service *svc, const char *store_path)
{
    char journal_path[512];
    FILE *file;
    char line[512];
    struct mem_service_store_import_state state;

    if (store_path == NULL || store_path[0] == '\0') {
        return 0;
    }
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        return -1;
    }
    file = fopen(journal_path, "r");
    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    mem_service_trim_line(line);
    if (strcmp(line, MEM_SERVICE_JOURNAL_MAGIC) != 0) {
        fclose(file);
        return -1;
    }

    memset(&state, 0, sizeof(state));
    while (fgets(line, sizeof(line), file) != NULL) {
        mem_service_trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (mem_service_import_store_line(svc, line, &state) != 0) {
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    return state.in_record || state.in_idempotency || state.in_audit ? -1 : 0;
}

static int mem_service_load_durable_store(struct mem_service *svc,
                                          const char *store_path)
{
    if (mem_service_load_store(svc, store_path) != 0) {
        return -1;
    }
    return mem_service_load_journal(svc, store_path);
}

static int mem_service_save_record(FILE *file, const struct mem_service_record *record)
{
    uint32_t i;

    if (!record->in_use) {
        return 0;
    }
    if (fprintf(file,
                "record_begin\n"
                "kind=%u\n"
                "key=%s\n"
                "request_id=%s\n"
                "prefix_group=%s\n"
                "group_id=%s\n"
                "block_hash=%s\n"
                "session_id=%s\n"
                "model_key=%s\n"
                "artifact_kind=%s\n"
                "artifact_id=%s\n"
                "placement_node=%u\n"
                "placement_level=%u\n"
                "hot_segment_id=%" PRIu64 "\n"
                "state=%u\n"
                "version=%" PRIu64 "\n"
                "last_result_segment=%" PRIu64 "\n"
                "object_owner_node=%u\n"
                "object_payload_kind=%u\n"
                "object_backing_offset=%" PRIu64 "\n"
                "object_backing_len=%" PRIu64 "\n"
                "object_payload_checksum=%" PRIu64 "\n"
                "object_publish_monotonic_ms=%" PRIu64 "\n"
                "object_publish_supernode_ms=%" PRIu64 "\n"
                "object_publish_supernode_offset_ms=%" PRId64 "\n"
                "member_count=%u\n",
                (uint32_t)record->kind,
                record->key,
                record->request_id,
                record->prefix_group,
                record->group_id,
                record->block_hash,
                record->session_id,
                record->model_key,
                record->artifact_kind,
                record->artifact_id,
                record->placement_node,
                record->placement_level,
                record->hot_segment_id,
                (uint32_t)record->state,
                record->version,
                record->last_result_segment,
                record->object_owner_node,
                record->object_payload_kind,
                record->object_backing_offset,
                record->object_backing_len,
                record->object_payload_checksum,
                record->object_publish_monotonic_ms,
                record->object_publish_supernode_ms,
                record->object_publish_supernode_offset_ms,
                record->member_count) < 0) {
        return -1;
    }
    for (i = 0; i < record->member_count && i < MEM_SERVICE_MAX_GROUP_MEMBERS; ++i) {
        if (fprintf(file, "member%u=%s\n", i, record->member_block_hashes[i]) < 0) {
            return -1;
        }
    }
    return fprintf(file, "record_end\n") < 0 ? -1 : 0;
}

static int mem_service_save_idempotency_record(
    FILE *file,
    const struct mem_service_idempotency_record *record)
{
    const char *cursor;
    const char *end;

    if (!record->in_use) {
        return 0;
    }
    if (fprintf(file,
                "idempotency_begin\n"
                "key=%s\n"
                "operation=%u\n"
                "request_checksum=%u\n"
                "status=%u\n"
                "response_len=%u\n",
                record->key,
                record->operation,
                record->request_checksum,
                record->status,
                record->response_len) < 0) {
        return -1;
    }
    cursor = record->response;
    end = record->response + record->response_len;
    while (cursor < end) {
        const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_len = newline != NULL ? (size_t)(newline - cursor)
                                          : (size_t)(end - cursor);

        if (fprintf(file, "response_line=%.*s\n", (int)line_len, cursor) < 0) {
            return -1;
        }
        if (newline == NULL) {
            break;
        }
        cursor = newline + 1;
    }
    return fprintf(file, "idempotency_end\n") < 0 ? -1 : 0;
}

static const struct mem_service_audit_event *mem_service_find_audit_sequence(
    const struct mem_service *svc,
    uint64_t sequence)
{
    size_t i;

    if (svc == NULL || sequence == 0) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_MAX_AUDIT_EVENTS; ++i) {
        const struct mem_service_audit_event *event = &svc->audit_events[i];

        if (event->in_use && event->sequence == sequence) {
            return event;
        }
    }
    return NULL;
}

static int mem_service_save_audit_event(FILE *file,
                                        const struct mem_service_audit_event *event)
{
    if (event == NULL || !event->in_use) {
        return 0;
    }
    return fprintf(file,
                   "audit_begin\n"
                   "sequence=%" PRIu64 "\n"
                   "monotonic_ms=%" PRIu64 "\n"
                   "operation=%u\n"
                   "status=%u\n"
                   "request_checksum=%u\n"
                   "response_checksum=%u\n"
                   "idempotency_replay=%u\n"
                   "key=%s\n"
                   "session_id=%s\n"
                   "model_key=%s\n"
                   "artifact_kind=%s\n"
                   "artifact_id=%s\n"
                   "idempotency_key=%s\n"
                   "version=%" PRIu64 "\n"
                   "checksum=%" PRIu64 "\n"
                   "audit_end\n",
                   event->sequence,
                   event->monotonic_ms,
                   event->operation,
                   event->status,
                   event->request_checksum,
                   event->response_checksum,
                   event->idempotency_replay,
                   event->key,
                   event->session_id,
                   event->model_key,
                   event->artifact_kind,
                   event->artifact_id,
                   event->idempotency_key,
                   event->version,
                   event->checksum) < 0
               ? -1
               : 0;
}

static int mem_service_journal_needs_header(const char *journal_path,
                                            bool *needs_header_out)
{
    FILE *file;
    char line[512];

    if (journal_path == NULL || needs_header_out == NULL) {
        return -1;
    }
    *needs_header_out = false;
    file = fopen(journal_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            *needs_header_out = true;
            return 0;
        }
        return -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        bool empty = feof(file);

        fclose(file);
        if (empty) {
            *needs_header_out = true;
            return 0;
        }
        return -1;
    }
    fclose(file);
    mem_service_trim_line(line);
    return strcmp(line, MEM_SERVICE_JOURNAL_MAGIC) == 0 ? 0 : -1;
}

static int mem_service_append_journal(
    const char *store_path,
    const struct mem_service_idempotency_record *idempotency,
    const struct mem_service_audit_event *event)
{
    char journal_path[512];
    FILE *file;
    bool needs_header = false;

    if (store_path == NULL || store_path[0] == '\0') {
        return 0;
    }
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0 ||
        mem_service_journal_needs_header(journal_path, &needs_header) != 0) {
        return -1;
    }
    file = fopen(journal_path, "a");
    if (file == NULL) {
        return -1;
    }
    if (needs_header && fprintf(file, "%s\n", MEM_SERVICE_JOURNAL_MAGIC) < 0) {
        fclose(file);
        return -1;
    }
    if (idempotency != NULL && idempotency->in_use &&
        mem_service_save_idempotency_record(file, idempotency) != 0) {
        fclose(file);
        return -1;
    }
    if (event != NULL && event->in_use &&
        mem_service_save_audit_event(file, event) != 0) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int mem_service_save_store(const struct mem_service *svc, const char *store_path)
{
    char tmp_path[512];
    FILE *file;
    size_t i;
    uint64_t first_sequence = 1;
    uint64_t sequence;

    if (store_path == NULL || store_path[0] == '\0') {
        return 0;
    }
    if (snprintf(tmp_path,
                 sizeof(tmp_path),
                 "%s.tmp.%ld",
                 store_path,
                 (long)getpid()) >= (int)sizeof(tmp_path)) {
        return -1;
    }
    file = fopen(tmp_path, "w");
    if (file == NULL) {
        return -1;
    }
    if (fprintf(file,
                "%s\n"
                "record_count=%zu\n"
                "audit_next_sequence=%" PRIu64 "\n"
                "audit_event_count=%" PRIu64 "\n",
                MEM_SERVICE_STORE_MAGIC,
                svc->record_count,
                svc->audit_next_sequence,
                svc->audit_event_count) < 0) {
        fclose(file);
        unlink(tmp_path);
        return -1;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (mem_service_save_record(file, &svc->records[i]) != 0) {
            fclose(file);
            unlink(tmp_path);
            return -1;
        }
    }
    for (i = 0; i < MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS; ++i) {
        if (mem_service_save_idempotency_record(file, &svc->idempotency_records[i]) != 0) {
            fclose(file);
            unlink(tmp_path);
            return -1;
        }
    }
    if (svc->audit_next_sequence > MEM_SERVICE_MAX_AUDIT_EVENTS + 1U) {
        first_sequence = svc->audit_next_sequence - MEM_SERVICE_MAX_AUDIT_EVENTS;
    }
    for (sequence = first_sequence; sequence < svc->audit_next_sequence; ++sequence) {
        const struct mem_service_audit_event *event =
            mem_service_find_audit_sequence(svc, sequence);

        if (event != NULL && mem_service_save_audit_event(file, event) != 0) {
            fclose(file);
            unlink(tmp_path);
            return -1;
        }
    }
    if (fclose(file) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, store_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static enum mem_service_wire_status mem_service_put_object(struct mem_service *svc,
                                                           const char *payload,
                                                           char *response,
                                                           size_t response_len,
                                                           const char *storage_root);

static bool mem_service_file_contains(const char *path, const char *needle)
{
    FILE *file;
    char line[512];

    if (path == NULL || needle == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(file);
            return true;
        }
    }
    fclose(file);
    return false;
}

int mem_service_run_store_fixture_check(void)
{
    static const char payload[] =
        "key=durable-fixture-object\n"
        "version=7\n"
        "checksum=12345\n"
        "backing_len=64\n"
        "idempotency_key=durable-fixture-idem\n";
    static const char conflict_payload[] =
        "key=durable-fixture-object\n"
        "version=7\n"
        "checksum=54321\n"
        "backing_len=64\n"
        "idempotency_key=durable-fixture-idem\n";
    struct mem_service first;
    struct mem_service second;
    struct mem_service_record record;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char replay_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char conflict_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[160];
    enum mem_service_wire_status status;
    enum mem_service_wire_status replay_status;
    enum mem_service_wire_status conflict_status;
    char journal_path[sizeof(store_path) + 16U];

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_store_fixture_%ld.store",
             (long)getpid());
    unlink(store_path);
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service store-fixtures: journal path failed\n");
        return 1;
    }
    unlink(journal_path);
    if (mem_service_init(&first, true, true, true) != 0 ||
        mem_service_init(&second, true, true, true) != 0) {
        fprintf(stderr, "mem_service store-fixtures: init failed\n");
        return 1;
    }
    status = mem_service_handle_operation(&first,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          payload,
                                          response,
                                          sizeof(response),
                                          store_path,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr, "mem_service store-fixtures: put failed status=%s\n",
                mem_service_wire_status_name(status));
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (!mem_service_file_contains(journal_path, MEM_SERVICE_JOURNAL_MAGIC) ||
        !mem_service_file_contains(journal_path, "idempotency_begin") ||
        !mem_service_file_contains(journal_path, "audit_begin") ||
        !mem_service_file_contains(journal_path, "key=durable-fixture-idem") ||
        !mem_service_file_contains(journal_path, "key=durable-fixture-object")) {
        fprintf(stderr, "mem_service store-fixtures: journal content mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_load_durable_store(&second, store_path) != 0) {
        fprintf(stderr, "mem_service store-fixtures: load failed path=%s\n", store_path);
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    if (mem_service_get_record(&second, "durable-fixture-object", &record) != 0 ||
        record.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
        record.version != 7 ||
        record.object_payload_checksum != 12345 ||
        record.object_backing_len != 64 ||
        second.audit_event_count != 1U) {
        fprintf(stderr, "mem_service store-fixtures: recovered record mismatch\n");
        return 1;
    }
    replay_status = mem_service_handle_operation(&second,
                                                MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                payload,
                                                replay_response,
                                                sizeof(replay_response),
                                                NULL,
                                                NULL);
    conflict_status = mem_service_handle_operation(&second,
                                                  MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                  conflict_payload,
                                                  conflict_response,
                                                  sizeof(conflict_response),
                                                  NULL,
                                                  NULL);
    if (replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
        conflict_status != MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT ||
        strcmp(response, replay_response) != 0 ||
        strstr(conflict_response, "idempotency_key=durable-fixture-idem\n") == NULL ||
        second.metrics.idempotency_replay_count != 1U ||
        second.metrics.idempotency_conflict_count != 1U) {
        fprintf(stderr, "mem_service store-fixtures: idempotency recovery mismatch\n");
        return 1;
    }
    printf("mem_service store-fixtures: status=ok records=%zu key=%s version=%" PRIu64
           " checksum=%" PRIu64 " idempotency_replay=%" PRIu64
           " journal_events=%" PRIu64 "\n",
           second.record_count,
           record.key,
           record.version,
           record.object_payload_checksum,
           second.metrics.idempotency_replay_count,
           second.audit_event_count);
    return 0;
}

int mem_service_run_journal_fixture_check(void)
{
    static const char payload[] =
        "key=journal-fixture-object\n"
        "version=9\n"
        "checksum=9009\n"
        "backing_len=96\n"
        "idempotency_key=journal-fixture-idem\n";
    struct mem_service writer;
    struct mem_service journal_only;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char replay_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[160];
    char journal_path[sizeof(store_path) + 16U];
    enum mem_service_wire_status status;
    enum mem_service_wire_status replay_status;
    uint64_t loaded_audit_events;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_journal_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service journal-fixtures: journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    if (mem_service_init(&writer, true, true, true) != 0 ||
        mem_service_init(&journal_only, true, true, true) != 0) {
        fprintf(stderr, "mem_service journal-fixtures: init failed\n");
        return 1;
    }
    status = mem_service_handle_operation(&writer,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          payload,
                                          response,
                                          sizeof(response),
                                          store_path,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr, "mem_service journal-fixtures: put failed status=%s\n",
                mem_service_wire_status_name(status));
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (!mem_service_file_contains(journal_path, MEM_SERVICE_JOURNAL_MAGIC) ||
        !mem_service_file_contains(journal_path, "idempotency_begin") ||
        !mem_service_file_contains(journal_path, "audit_begin") ||
        !mem_service_file_contains(journal_path, "key=journal-fixture-idem") ||
        !mem_service_file_contains(journal_path, "key=journal-fixture-object")) {
        fprintf(stderr, "mem_service journal-fixtures: journal content mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_load_journal(&journal_only, store_path) != 0) {
        fprintf(stderr, "mem_service journal-fixtures: journal load failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    loaded_audit_events = journal_only.audit_event_count;
    replay_status = mem_service_handle_operation(&journal_only,
                                                MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                payload,
                                                replay_response,
                                                sizeof(replay_response),
                                                NULL,
                                                NULL);
    unlink(store_path);
    unlink(journal_path);
    if (loaded_audit_events != 1U ||
        journal_only.audit_event_count != 2U ||
        replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strcmp(response, replay_response) != 0 ||
        journal_only.metrics.idempotency_replay_count != 1U) {
        fprintf(stderr,
                "mem_service journal-fixtures: replay recovery mismatch\n");
        return 1;
    }
    printf("mem_service journal-fixtures: status=ok journal_magic=%s "
           "loaded_audit_events=%" PRIu64 " replay_audit_events=%" PRIu64
           " idempotency_replay=%" PRIu64 "\n",
           MEM_SERVICE_JOURNAL_MAGIC,
           loaded_audit_events,
           journal_only.audit_event_count,
           journal_only.metrics.idempotency_replay_count);
    return 0;
}

int mem_service_run_upgrade_rollback_runtime_fixture_check(void)
{
    static const char baseline_object_payload[] =
        "key=upgrade-fixture-object\n"
        "version=11\n"
        "checksum=11011\n"
        "backing_len=128\n"
        "idempotency_key=upgrade-fixture-object-v11\n";
    static const char upgraded_object_payload[] =
        "key=upgrade-fixture-object\n"
        "version=12\n"
        "checksum=12012\n"
        "backing_len=256\n"
        "idempotency_key=upgrade-fixture-object-v12\n";
    static const char upgraded_object_query[] =
        "key=upgrade-fixture-object\n"
        "expected_version=12\n"
        "expected_checksum=12012\n";
    static const char training_bad_version_query[] =
        "key=training/upgrade-run/step-7\n"
        "expected_session_id=upgrade-run\n"
        "expected_model_key=upgrade-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=step-7\n"
        "expected_version=8\n"
        "expected_checksum=7007\n";
    static const char training_commit_payload[] =
        "key=training/upgrade-run/step-7\n"
        "session_id=upgrade-run\n"
        "model_key=upgrade-model\n"
        "artifact_kind=training-step-commit\n"
        "artifact_id=step-7\n"
        "owner=3\n"
        "payload_kind=4\n"
        "backing_offset=0\n"
        "backing_len=8\n"
        "checksum=7007\n"
        "version=7\n"
        "idempotency_key=upgrade-fixture-training-step-7\n";
    static const char training_commit_query[] =
        "key=training/upgrade-run/step-7\n"
        "expected_session_id=upgrade-run\n"
        "expected_model_key=upgrade-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=step-7\n"
        "expected_version=7\n"
        "expected_checksum=7007\n";
    static const char training_bad_checksum_query[] =
        "key=training/upgrade-run/step-7\n"
        "expected_session_id=upgrade-run\n"
        "expected_model_key=upgrade-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=step-7\n"
        "expected_version=7\n"
        "expected_checksum=7008\n";
    static const char bad_generation_snapshot[] =
        "mem_service_store_v0\n"
        "record_count=0\n"
        "audit_next_sequence=1\n"
        "audit_event_count=0\n";
    static struct mem_service current;
    static struct mem_service restarted;
    static struct mem_service upgraded;
    static struct mem_service rolled_back;
    static struct mem_service rejected;
    struct mem_service_record record;
    char store_path[160];
    char journal_path[sizeof(store_path) + 16U];
    char baseline_object_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char baseline_training_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char replay_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char upgraded_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char upgraded_query_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char training_query_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char baseline_snapshot[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char restore_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char rollback_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char fail_closed_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status;
    enum mem_service_wire_status replay_status;
    enum mem_service_wire_status upgraded_status;
    enum mem_service_wire_status upgraded_query_status;
    enum mem_service_wire_status training_query_status;
    enum mem_service_wire_status restore_status;
    enum mem_service_wire_status rollback_status;
    enum mem_service_wire_status stale_status;
    enum mem_service_wire_status checksum_status;
    enum mem_service_wire_status reject_status;
    int failures = 0;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_upgrade_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    if (mem_service_init(&current, true, true, true) != 0 ||
        mem_service_init(&restarted, true, true, true) != 0 ||
        mem_service_init(&upgraded, true, true, true) != 0 ||
        mem_service_init(&rolled_back, true, true, true) != 0 ||
        mem_service_init(&rejected, true, true, true) != 0) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: init failed\n");
        return 1;
    }

    status = mem_service_handle_operation(&current,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          baseline_object_payload,
                                          baseline_object_response,
                                          sizeof(baseline_object_response),
                                          store_path,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: baseline object failed status=%s\n",
                mem_service_wire_status_name(status));
        failures -= 1;
    }
    status = mem_service_handle_operation(&current,
                                          MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                          training_commit_payload,
                                          baseline_training_response,
                                          sizeof(baseline_training_response),
                                          store_path,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: training commit failed status=%s\n",
                mem_service_wire_status_name(status));
        failures -= 1;
    }
    if (!mem_service_file_contains(journal_path, MEM_SERVICE_JOURNAL_MAGIC) ||
        !mem_service_file_contains(journal_path, "idempotency_begin") ||
        !mem_service_file_contains(journal_path, "audit_begin") ||
        !mem_service_file_contains(journal_path, "key=upgrade-fixture-object-v11") ||
        !mem_service_file_contains(journal_path,
                                   "key=upgrade-fixture-training-step-7")) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: journal gate mismatch\n");
        failures -= 1;
    }
    if (mem_service_load_durable_store(&restarted, store_path) != 0 ||
        mem_service_get_record(&restarted, "upgrade-fixture-object", &record) != 0 ||
        record.version != 11U || record.object_payload_checksum != 11011U ||
        mem_service_get_record(&restarted, "training/upgrade-run/step-7", &record) != 0 ||
        record.version != 7U || record.object_payload_checksum != 7007U ||
        strcmp(record.artifact_kind, "training-step-commit") != 0) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: restart recovery mismatch\n");
        failures -= 1;
    }

    status = mem_service_handle_operation(&restarted,
                                          MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                          "",
                                          baseline_snapshot,
                                          sizeof(baseline_snapshot),
                                          NULL,
                                          NULL);
    restore_status = mem_service_handle_operation(&upgraded,
                                                  MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                                  baseline_snapshot,
                                                  restore_response,
                                                  sizeof(restore_response),
                                                  NULL,
                                                  NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        restore_status != MEM_SERVICE_WIRE_STATUS_OK ||
        upgraded.record_count != 2U) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: same-version restore mismatch\n");
        failures -= 1;
    }

    replay_status = mem_service_handle_operation(&upgraded,
                                                 MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                 baseline_object_payload,
                                                 replay_response,
                                                 sizeof(replay_response),
                                                 NULL,
                                                 NULL);
    upgraded_status = mem_service_handle_operation(&upgraded,
                                                  MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                  upgraded_object_payload,
                                                  upgraded_response,
                                                  sizeof(upgraded_response),
                                                  NULL,
                                                  NULL);
    upgraded_query_status = mem_service_handle_operation(&upgraded,
                                                        MEM_SERVICE_WIRE_OP_GET_OBJECT,
                                                        upgraded_object_query,
                                                        upgraded_query_response,
                                                        sizeof(upgraded_query_response),
                                                        NULL,
                                                        NULL);
    training_query_status = mem_service_handle_operation(&upgraded,
                                                        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                                        training_commit_query,
                                                        training_query_response,
                                                        sizeof(training_query_response),
                                                        NULL,
                                                        NULL);
    if (replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strcmp(baseline_object_response, replay_response) != 0 ||
        upgraded.metrics.idempotency_replay_count != 1U ||
        upgraded_status != MEM_SERVICE_WIRE_STATUS_OK ||
        upgraded_query_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(upgraded_query_response, "version=12\n") == NULL ||
        training_query_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(training_query_response, "artifact_kind=training-step-commit\n") == NULL) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: upgraded runtime mismatch\n");
        failures -= 1;
    }

    rollback_status = mem_service_handle_operation(&rolled_back,
                                                  MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                                  baseline_snapshot,
                                                  rollback_response,
                                                  sizeof(rollback_response),
                                                  NULL,
                                                  NULL);
    stale_status = mem_service_handle_operation(&rolled_back,
                                                MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                                training_bad_version_query,
                                                fail_closed_response,
                                                sizeof(fail_closed_response),
                                                NULL,
                                                NULL);
    checksum_status = mem_service_handle_operation(&rolled_back,
                                                   MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                                   training_bad_checksum_query,
                                                   fail_closed_response,
                                                   sizeof(fail_closed_response),
                                                   NULL,
                                                   NULL);
    reject_status = mem_service_handle_operation(&rejected,
                                                 MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                                 bad_generation_snapshot,
                                                 fail_closed_response,
                                                 sizeof(fail_closed_response),
                                                 NULL,
                                                 NULL);
    if (rollback_status != MEM_SERVICE_WIRE_STATUS_OK ||
        mem_service_get_record(&rolled_back, "upgrade-fixture-object", &record) != 0 ||
        record.version != 11U || record.object_payload_checksum != 11011U ||
        stale_status != MEM_SERVICE_WIRE_STATUS_STALE_REF ||
        checksum_status != MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH ||
        reject_status != MEM_SERVICE_WIRE_STATUS_INVALID_SESSION ||
        rolled_back.metrics.stale_ref_count != 1U ||
        rolled_back.metrics.checksum_mismatch_count != 1U ||
        rolled_back.metrics.fail_closed_count != 2U) {
        fprintf(stderr,
                "mem_service upgrade-rollback-runtime-fixtures: rollback/admission mismatch\n");
        failures -= 1;
    }

    unlink(store_path);
    unlink(journal_path);
    if (failures != 0) {
        return 1;
    }
    printf("mem_service upgrade-rollback-runtime-fixtures: status=ok "
           "same_version_restart=store-snapshot+journal "
           "same_version_upgrade=export-snapshot+restore-snapshot "
           "same_version_rollback=baseline-snapshot-restore "
           "serving_records=1 pretraining_commits=1 idempotency_replay=%" PRIu64
           " fail_closed=%" PRIu64
           " release_admission=reject-unknown-release-generation\n",
           upgraded.metrics.idempotency_replay_count,
           rolled_back.metrics.fail_closed_count);
    return 0;
}

int mem_service_run_compat_runtime_fixture_check(void)
{
    static const char old_object_payload[] =
        "key=compat-old-object\n"
        "version=1\n"
        "checksum=101\n"
        "backing_len=64\n"
        "idempotency_key=compat-old-object-v1\n";
    static const char old_object_conflict_payload[] =
        "key=compat-old-object\n"
        "version=1\n"
        "checksum=202\n"
        "backing_len=64\n"
        "idempotency_key=compat-old-object-v1\n";
    static const char old_object_query[] =
        "key=compat-old-object\n";
    static const char old_runtime_payload[] =
        "key=runtime/compat-old/session-a/range-0\n"
        "session_id=compat-session-a\n"
        "model_key=compat-model\n"
        "artifact_kind=hidden-range\n"
        "artifact_id=range-0\n"
        "checksum=7707\n"
        "version=7\n"
        "idempotency_key=compat-old-runtime-range-0-v7\n";
    static const char current_runtime_query[] =
        "key=runtime/compat-old/session-a/range-0\n"
        "expected_session_id=compat-session-a\n"
        "expected_model_key=compat-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=7\n"
        "expected_checksum=7707\n";
    static const char bad_model_query[] =
        "key=runtime/compat-old/session-a/range-0\n"
        "expected_session_id=compat-session-a\n"
        "expected_model_key=wrong-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=7\n"
        "expected_checksum=7707\n";
    static const char stale_runtime_query[] =
        "key=runtime/compat-old/session-a/range-0\n"
        "expected_session_id=compat-session-a\n"
        "expected_model_key=compat-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=8\n"
        "expected_checksum=7707\n";
    static const char checksum_runtime_query[] =
        "key=runtime/compat-old/session-a/range-0\n"
        "expected_session_id=compat-session-a\n"
        "expected_model_key=compat-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=7\n"
        "expected_checksum=7708\n";
    static const char current_execution_payload[] =
        "key=execution/compat-current/session-a/logits-0\n"
        "session_id=compat-session-a\n"
        "request_id=req-current-0\n"
        "model_key=compat-model\n"
        "artifact_kind=logits\n"
        "artifact_id=logits-0\n"
        "owner=2\n"
        "payload_kind=3\n"
        "backing_offset=64\n"
        "backing_len=256\n"
        "checksum=8808\n"
        "version=8\n"
        "future_optional_field=ignored\n"
        "idempotency_key=compat-current-execution-logits-0-v8\n";
    static const char old_execution_query[] =
        "key=execution/compat-current/session-a/logits-0\n";
    static const char old_training_payload[] =
        "key=training/compat-run/global-step-42/commit\n"
        "session_id=compat-run\n"
        "model_key=compat-model\n"
        "artifact_kind=training-step-commit\n"
        "artifact_id=global-step-42\n"
        "checksum=4242\n"
        "version=42\n"
        "idempotency_key=compat-old-training-step-42-v42\n";
    static const char current_training_query[] =
        "key=training/compat-run/global-step-42/commit\n"
        "expected_session_id=compat-run\n"
        "expected_model_key=compat-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_version=42\n"
        "expected_checksum=4242\n";
    static struct mem_service server;
    char old_object_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char old_object_replay[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char old_object_conflict[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char old_object_get[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char old_runtime_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char current_runtime_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char fail_closed_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char current_execution_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char old_execution_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char old_training_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char current_training_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status old_object_status;
    enum mem_service_wire_status old_object_replay_status;
    enum mem_service_wire_status old_object_conflict_status;
    enum mem_service_wire_status old_object_get_status;
    enum mem_service_wire_status old_runtime_status;
    enum mem_service_wire_status current_runtime_status;
    enum mem_service_wire_status bad_model_status;
    enum mem_service_wire_status stale_runtime_status;
    enum mem_service_wire_status checksum_runtime_status;
    enum mem_service_wire_status current_execution_status;
    enum mem_service_wire_status old_execution_status;
    enum mem_service_wire_status old_training_status;
    enum mem_service_wire_status current_training_status;
    int failures = 0;

    if (mem_service_init(&server, true, true, true) != 0) {
        fprintf(stderr, "mem_service compat-runtime-fixtures: init failed\n");
        return 1;
    }

    old_object_status = mem_service_handle_operation(&server,
                                                     MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                     old_object_payload,
                                                     old_object_response,
                                                     sizeof(old_object_response),
                                                     NULL,
                                                     NULL);
    old_object_replay_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                     old_object_payload,
                                     old_object_replay,
                                     sizeof(old_object_replay),
                                     NULL,
                                     NULL);
    old_object_conflict_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                     old_object_conflict_payload,
                                     old_object_conflict,
                                     sizeof(old_object_conflict),
                                     NULL,
                                     NULL);
    old_object_get_status = mem_service_handle_operation(&server,
                                                        MEM_SERVICE_WIRE_OP_GET_OBJECT,
                                                        old_object_query,
                                                        old_object_get,
                                                        sizeof(old_object_get),
                                                        NULL,
                                                        NULL);
    if (old_object_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_object_replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strcmp(old_object_response, old_object_replay) != 0 ||
        old_object_conflict_status != MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT ||
        old_object_get_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(old_object_get, "version=1\n") == NULL ||
        strstr(old_object_get, "object_payload_checksum=101\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-runtime-fixtures: old object path mismatch\n");
        failures -= 1;
    }

    old_runtime_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                     old_runtime_payload,
                                     old_runtime_response,
                                     sizeof(old_runtime_response),
                                     NULL,
                                     NULL);
    current_runtime_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     current_runtime_query,
                                     current_runtime_response,
                                     sizeof(current_runtime_response),
                                     NULL,
                                     NULL);
    bad_model_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     bad_model_query,
                                     fail_closed_response,
                                     sizeof(fail_closed_response),
                                     NULL,
                                     NULL);
    stale_runtime_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     stale_runtime_query,
                                     fail_closed_response,
                                     sizeof(fail_closed_response),
                                     NULL,
                                     NULL);
    checksum_runtime_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     checksum_runtime_query,
                                     fail_closed_response,
                                     sizeof(fail_closed_response),
                                     NULL,
                                     NULL);
    if (old_runtime_status != MEM_SERVICE_WIRE_STATUS_OK ||
        current_runtime_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(current_runtime_response, "artifact_kind=hidden-range\n") == NULL ||
        strstr(current_runtime_response, "version=7\n") == NULL ||
        strstr(current_runtime_response, "object_payload_checksum=7707\n") == NULL ||
        bad_model_status != MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING ||
        stale_runtime_status != MEM_SERVICE_WIRE_STATUS_STALE_REF ||
        checksum_runtime_status != MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH) {
        fprintf(stderr,
                "mem_service compat-runtime-fixtures: runtime path mismatch\n");
        failures -= 1;
    }

    current_execution_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
                                     current_execution_payload,
                                     current_execution_response,
                                     sizeof(current_execution_response),
                                     NULL,
                                     NULL);
    old_execution_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
                                     old_execution_query,
                                     old_execution_response,
                                     sizeof(old_execution_response),
                                     NULL,
                                     NULL);
    if (current_execution_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_execution_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(old_execution_response, "artifact_kind=logits\n") == NULL ||
        strstr(old_execution_response, "version=8\n") == NULL ||
        strstr(old_execution_response, "object_payload_checksum=8808\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-runtime-fixtures: current execution path mismatch\n");
        failures -= 1;
    }

    old_training_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                     old_training_payload,
                                     old_training_response,
                                     sizeof(old_training_response),
                                     NULL,
                                     NULL);
    current_training_status =
        mem_service_handle_operation(&server,
                                     MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                     current_training_query,
                                     current_training_response,
                                     sizeof(current_training_response),
                                     NULL,
                                     NULL);
    if (old_training_status != MEM_SERVICE_WIRE_STATUS_OK ||
        current_training_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(current_training_response,
               "artifact_kind=training-step-commit\n") == NULL ||
        strstr(current_training_response, "version=42\n") == NULL ||
        strstr(current_training_response, "object_payload_checksum=4242\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-runtime-fixtures: pretraining path mismatch\n");
        failures -= 1;
    }

    if (server.metrics.idempotency_replay_count != 1U ||
        server.metrics.idempotency_conflict_count != 1U ||
        server.metrics.invalid_model_binding_count != 1U ||
        server.metrics.stale_ref_count != 1U ||
        server.metrics.checksum_mismatch_count != 1U ||
        server.metrics.fail_closed_count != 4U ||
        server.audit_event_count != 9U) {
        fprintf(stderr,
                "mem_service compat-runtime-fixtures: metrics/audit mismatch "
                "replay=%" PRIu64 " conflict=%" PRIu64 " invalid_model=%" PRIu64
                " stale=%" PRIu64 " checksum=%" PRIu64 " fail_closed=%" PRIu64
                " audit=%" PRIu64 "\n",
                server.metrics.idempotency_replay_count,
                server.metrics.idempotency_conflict_count,
                server.metrics.invalid_model_binding_count,
                server.metrics.stale_ref_count,
                server.metrics.checksum_mismatch_count,
                server.metrics.fail_closed_count,
                server.audit_event_count);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }

    printf("mem_service compat-runtime-fixtures: status=ok "
           "old_v1_client_current_server=runtime-compatible "
           "current_v1_client_current_server=runtime-compatible "
           "serving_paths=object,runtime-handoff,execution-artifact "
           "pretraining_commits=1 idempotency_replay=%" PRIu64
           " idempotency_conflict=%" PRIu64 " fail_closed=%" PRIu64
           " old_server_runtime_binary=not-in-tree\n",
           server.metrics.idempotency_replay_count,
           server.metrics.idempotency_conflict_count,
           server.metrics.fail_closed_count);
    return 0;
}

int mem_service_run_durable_catalog_fixture_check(void)
{
    char storage_root[160];
    char catalog_dir[192];
    char block_dir[192];
    char quarantine_dir[192];
    char manifest_path[224];
    char store_path[224];
    char journal_path[240];

    snprintf(storage_root,
             sizeof(storage_root),
             "/tmp/linqu_mem_service_catalog_fixture_%ld",
             (long)getpid());
    if (mem_service_join_path(catalog_dir,
                              sizeof(catalog_dir),
                              storage_root,
                              "catalog") != 0 ||
        mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        mem_service_make_catalog_path(storage_root,
                                      MEM_SERVICE_DURABLE_CATALOG_MANIFEST,
                                      manifest_path,
                                      sizeof(manifest_path)) != 0 ||
        mem_service_make_catalog_path(storage_root,
                                      "store.snapshot",
                                      store_path,
                                      sizeof(store_path)) != 0 ||
        mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service durable-catalog-fixtures: path setup failed\n");
        return 1;
    }

    unlink(manifest_path);
    unlink(store_path);
    unlink(journal_path);
    rmdir(quarantine_dir);
    rmdir(block_dir);
    rmdir(catalog_dir);
    rmdir(storage_root);

    if (mem_service_prepare_durable_catalog_layout(storage_root) != 0 ||
        mem_service_write_durable_catalog_manifest(storage_root, store_path) != 0) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: catalog prepare failed\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        return 1;
    }
    if (!mem_service_path_is_dir(catalog_dir) ||
        !mem_service_path_is_dir(block_dir) ||
        !mem_service_path_is_dir(quarantine_dir) ||
        !mem_service_file_contains(manifest_path,
                                   MEM_SERVICE_DURABLE_CATALOG_MAGIC) ||
        !mem_service_file_contains(manifest_path, "layout=storage-root-v1") ||
        !mem_service_file_contains(manifest_path,
                                   "payload_block_backend=sealed-local-block-v1") ||
        !mem_service_file_contains(manifest_path,
                                   "corrupt_payload_policy=quarantine-fail-closed") ||
        !mem_service_file_contains(manifest_path, store_path) ||
        !mem_service_file_contains(manifest_path, journal_path)) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: catalog content mismatch\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        return 1;
    }
    unlink(manifest_path);
    rmdir(quarantine_dir);
    rmdir(block_dir);
    rmdir(catalog_dir);
    rmdir(storage_root);
    printf("mem_service durable-catalog-fixtures: status=ok layout=storage-root-v1 "
           "manifest=%s store=%s payload_block_backend=sealed-local-block-v1\n",
           MEM_SERVICE_DURABLE_CATALOG_MANIFEST,
           "catalog/store.snapshot");
    return 0;
}

static bool mem_service_payload_get_u64_checked(const char *payload,
                                                const char *name,
                                                uint64_t *out)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);

    return mem_service_wire_payload_get_u64_checked(&view, name, out);
}

static enum mem_service_kvcache_state mem_service_payload_get_state(
    const char *payload,
    enum mem_service_kvcache_state default_value)
{
    char state[32];

    if (!mem_service_payload_get_string(payload, "state", state, sizeof(state))) {
        return default_value;
    }
    if (strcmp(state, "missing") == 0) {
        return MEM_SERVICE_KVCACHE_STATE_MISSING;
    }
    if (strcmp(state, "filled") == 0) {
        return MEM_SERVICE_KVCACHE_STATE_FILLED;
    }
    if (strcmp(state, "hot") == 0) {
        return MEM_SERVICE_KVCACHE_STATE_HOT;
    }
    if (strcmp(state, "reloaded") == 0) {
        return MEM_SERVICE_KVCACHE_STATE_RELOADED;
    }
    return default_value;
}

static const char *mem_service_record_session_id(const struct mem_service_record *record)
{
    return record->session_id[0] != '\0' ? record->session_id : record->request_id;
}

static const char *mem_service_record_model_key(const struct mem_service_record *record)
{
    return record->model_key[0] != '\0' ? record->model_key : record->prefix_group;
}

static const char *mem_service_record_artifact_kind(const struct mem_service_record *record)
{
    return record->artifact_kind[0] != '\0' ? record->artifact_kind : record->group_id;
}

static const char *mem_service_record_artifact_id(const struct mem_service_record *record)
{
    return record->artifact_id[0] != '\0' ? record->artifact_id : record->block_hash;
}

static void mem_service_format_record_payload(const struct mem_service_record *record,
                                              char *out,
                                              size_t out_len)
{
    snprintf(out,
             out_len,
             "key=%s\nkind=%u\nrequest_id=%s\nprefix_group=%s\ngroup_id=%s\n"
             "session_id=%s\nmodel_key=%s\nartifact_kind=%s\nartifact_id=%s\n"
             "block_hash=%s\nplacement_node=%u\nplacement_level=%u\n"
             "hot_segment_id=%" PRIu64 "\nstate=%s\nversion=%" PRIu64 "\n"
             "last_result_segment=%" PRIu64 "\nobject_owner_node=%u\n"
             "object_payload_kind=%u\nobject_backing_offset=%" PRIu64 "\n"
             "object_backing_len=%" PRIu64 "\nobject_payload_checksum=%" PRIu64 "\n",
             record->key,
             (uint32_t)record->kind,
             record->request_id,
             record->prefix_group,
             record->group_id,
             mem_service_record_session_id(record),
             mem_service_record_model_key(record),
             mem_service_record_artifact_kind(record),
             mem_service_record_artifact_id(record),
             record->block_hash,
             record->placement_node,
             record->placement_level,
             record->hot_segment_id,
             mem_service_kvcache_state_name(record->state),
             record->version,
             record->last_result_segment,
             record->object_owner_node,
             record->object_payload_kind,
             record->object_backing_offset,
             record->object_backing_len,
             record->object_payload_checksum);
}

static void mem_service_format_inspect_record_payload(
    const struct mem_service_record *record,
    char *out,
    size_t out_len)
{
    snprintf(out,
             out_len,
             "key=%s\nkind=%u\nkind_name=%s\nrequest_id=%s\nprefix_group=%s\n"
             "group_id=%s\nsession_id=%s\nmodel_key=%s\nartifact_kind=%s\n"
             "artifact_id=%s\nblock_hash=%s\nplacement_node=%u\n"
             "placement_level=%u\nhot_segment_id=%" PRIu64 "\nstate=%s\n"
             "version=%" PRIu64 "\nlast_result_segment=%" PRIu64 "\n"
             "object_owner_node=%u\nobject_payload_kind=%u\n"
             "object_backing_offset=%" PRIu64 "\nobject_backing_len=%" PRIu64 "\n"
             "object_payload_checksum=%" PRIu64 "\n"
             "member_count=%u\n",
             record->key,
             (uint32_t)record->kind,
             mem_service_record_kind_name(record->kind),
             record->request_id,
             record->prefix_group,
             record->group_id,
             mem_service_record_session_id(record),
             mem_service_record_model_key(record),
             mem_service_record_artifact_kind(record),
             mem_service_record_artifact_id(record),
             record->block_hash,
             record->placement_node,
             record->placement_level,
             record->hot_segment_id,
             mem_service_kvcache_state_name(record->state),
             record->version,
             record->last_result_segment,
             record->object_owner_node,
             record->object_payload_kind,
             record->object_backing_offset,
             record->object_backing_len,
             record->object_payload_checksum,
             record->member_count);
}

static enum mem_service_wire_status mem_service_put_object(struct mem_service *svc,
                                                           const char *payload,
                                                           char *response,
                                                           size_t response_len,
                                                           const char *storage_root)
{
    struct mem_service_record *record;
    struct mem_service_record next;
    char key[sizeof(record->key)];
    char payload_inline[1024];
    char payload_path[512];
    enum mem_service_wire_status block_status;

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    memset(&next, 0, sizeof(next));
    next.in_use = true;
    next.kind = MEM_SERVICE_RECORD_KVCACHE_OBJECT;
    snprintf(next.key, sizeof(next.key), "%s", key);
    next.version = mem_service_payload_get_u64(payload, "version", 1);
    next.object_owner_node = mem_service_payload_get_u32(payload, "owner", 0);
    next.object_payload_kind = mem_service_payload_get_u32(payload, "payload_kind", 0);
    next.object_backing_offset = mem_service_payload_get_u64(payload, "backing_offset", 0);
    next.object_backing_len = mem_service_payload_get_u64(payload, "backing_len", 0);
    next.object_payload_checksum = mem_service_payload_get_u64(payload, "checksum", 0);
    next.object_publish_monotonic_ms = mem_service_wall_clock_ms();
    payload_inline[0] = '\0';
    payload_path[0] = '\0';
    (void)mem_service_payload_get_string(payload,
                                         "payload_inline",
                                         payload_inline,
                                         sizeof(payload_inline));
    (void)mem_service_payload_get_string(payload,
                                         "payload_path",
                                         payload_path,
                                         sizeof(payload_path));
    if (payload_inline[0] != '\0' || payload_path[0] != '\0') {
        block_status = mem_service_write_payload_block(storage_root,
                                                       payload,
                                                       payload_inline,
                                                       payload_path,
                                                       &next);
        if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
            return block_status;
        }
    }
    record = mem_service_find_record(svc, key);
    if (record == NULL) {
        record = mem_service_alloc_record(svc);
        if (record == NULL) {
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
    }
    *record = next;
    snprintf(response,
             response_len,
             "status=ok\nkey=%s\nversion=%" PRIu64 "\n",
             record->key,
             record->version);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_get_object(struct mem_service *svc,
                                                           const char *payload,
                                                           char *response,
                                                           size_t response_len,
                                                           const char *storage_root)
{
    struct mem_service_record record;
    char key[sizeof(record.key)];
    enum mem_service_wire_status block_status;

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_get_record(svc, key, &record) != 0 ||
        record.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    block_status = mem_service_validate_payload_block(storage_root, &record);
    if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return block_status;
    }
    mem_service_format_record_payload(&record, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_inspect_object(struct mem_service *svc,
                                                               const char *payload,
                                                               char *response,
                                                               size_t response_len,
                                                               const char *storage_root)
{
    struct mem_service_record record;
    char key[sizeof(record.key)];
    enum mem_service_wire_status block_status;

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_get_record(svc, key, &record) != 0 ||
        record.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    block_status = mem_service_validate_payload_block(storage_root, &record);
    if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return block_status;
    }
    mem_service_format_inspect_record_payload(&record, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static void mem_service_set_optional_record_string(const char *payload,
                                                   const char *field_name,
                                                   char *out,
                                                   size_t out_len)
{
    char value[96];

    if (mem_service_payload_get_string(payload, field_name, value, sizeof(value))) {
        size_t copy_len = strlen(value);

        if (out_len == 0) {
            return;
        }
        if (copy_len >= out_len) {
            copy_len = out_len - 1;
        }
        memcpy(out, value, copy_len);
        out[copy_len] = '\0';
    }
}

static bool mem_service_payload_string_mismatch(const char *payload,
                                                const char *field_name,
                                                const char *actual)
{
    char expected[96];

    if (!mem_service_payload_get_string(payload,
                                        field_name,
                                        expected,
                                        sizeof(expected))) {
        return false;
    }
    return strcmp(actual, expected) != 0;
}

static enum mem_service_wire_status mem_service_store_artifact(
    struct mem_service *svc,
    const char *payload,
    enum mem_service_record_kind record_kind,
    char *response,
    size_t response_len,
    const char *storage_root)
{
    struct mem_service_record *record;
    struct mem_service_record next;
    char key[sizeof(record->key)];
    char payload_inline[1024];
    char payload_path[512];
    uint64_t old_version = 0;
    uint64_t requested_version = 0;
    bool has_requested_version;
    enum mem_service_wire_status block_status;

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    record = mem_service_find_record(svc, key);
    if (record != NULL) {
        old_version = record->version;
    }
    has_requested_version =
        mem_service_payload_get_u64_checked(payload, "version", &requested_version);
    memset(&next, 0, sizeof(next));
    next.in_use = true;
    next.kind = record_kind;
    snprintf(next.key, sizeof(next.key), "%s", key);
    next.version = has_requested_version ? requested_version : old_version + 1U;
    if (next.version == 0) {
        next.version = 1;
    }
    mem_service_set_optional_record_string(payload,
                                           "session_id",
                                           next.session_id,
                                           sizeof(next.session_id));
    mem_service_set_optional_record_string(payload,
                                           "request_id",
                                           next.request_id,
                                           sizeof(next.request_id));
    mem_service_set_optional_record_string(payload,
                                           "model_key",
                                           next.model_key,
                                           sizeof(next.model_key));
    mem_service_set_optional_record_string(payload,
                                           "artifact_kind",
                                           next.artifact_kind,
                                           sizeof(next.artifact_kind));
    mem_service_set_optional_record_string(payload,
                                           "artifact_id",
                                           next.artifact_id,
                                           sizeof(next.artifact_id));
    mem_service_set_optional_record_string(payload,
                                           "block_hash",
                                           next.block_hash,
                                           sizeof(next.block_hash));
    if (next.prefix_group[0] == '\0' && next.model_key[0] != '\0') {
        snprintf(next.prefix_group, sizeof(next.prefix_group), "%s", next.model_key);
    }
    if (next.group_id[0] == '\0' && next.artifact_kind[0] != '\0') {
        snprintf(next.group_id, sizeof(next.group_id), "%s", next.artifact_kind);
    }
    if (next.block_hash[0] == '\0' && next.artifact_id[0] != '\0') {
        snprintf(next.block_hash, sizeof(next.block_hash), "%s", next.artifact_id);
    }
    next.object_owner_node = mem_service_payload_get_u32(payload, "owner", 0);
    next.object_payload_kind = mem_service_payload_get_u32(payload, "payload_kind", 0);
    next.object_backing_offset = mem_service_payload_get_u64(payload, "backing_offset", 0);
    next.object_backing_len = mem_service_payload_get_u64(payload, "backing_len", 0);
    next.object_payload_checksum = mem_service_payload_get_u64(payload, "checksum", 0);
    next.object_publish_monotonic_ms = mem_service_wall_clock_ms();
    payload_inline[0] = '\0';
    payload_path[0] = '\0';
    (void)mem_service_payload_get_string(payload,
                                         "payload_inline",
                                         payload_inline,
                                         sizeof(payload_inline));
    (void)mem_service_payload_get_string(payload,
                                         "payload_path",
                                         payload_path,
                                         sizeof(payload_path));
    if (payload_inline[0] != '\0' || payload_path[0] != '\0') {
        block_status = mem_service_write_payload_block(storage_root,
                                                       payload,
                                                       payload_inline,
                                                       payload_path,
                                                       &next);
        if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
            return block_status;
        }
    }
    if (record == NULL) {
        record = mem_service_alloc_record(svc);
        if (record == NULL) {
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
    }
    *record = next;
    mem_service_format_record_payload(record, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_query_artifact(
    struct mem_service *svc,
    const char *payload,
    enum mem_service_record_kind record_kind,
    char *response,
    size_t response_len,
    const char *storage_root)
{
    struct mem_service_record record;
    char key[sizeof(record.key)];
    uint64_t expected_version;
    uint64_t expected_checksum;
    enum mem_service_wire_status block_status;

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_get_record(svc, key, &record) != 0 || record.kind != record_kind) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    if (mem_service_payload_string_mismatch(payload,
                                            "expected_session_id",
                                            mem_service_record_session_id(&record))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_payload_string_mismatch(payload,
                                            "expected_model_key",
                                            mem_service_record_model_key(&record))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING;
    }
    if (mem_service_payload_string_mismatch(payload,
                                            "expected_artifact_kind",
                                            mem_service_record_artifact_kind(&record)) ||
        mem_service_payload_string_mismatch(payload,
                                            "expected_artifact_id",
                                            mem_service_record_artifact_id(&record))) {
        return MEM_SERVICE_WIRE_STATUS_STALE_REF;
    }
    if (mem_service_payload_get_u64_checked(payload,
                                            "expected_version",
                                            &expected_version) &&
        record.version != expected_version) {
        return MEM_SERVICE_WIRE_STATUS_STALE_REF;
    }
    if (mem_service_payload_get_u64_checked(payload,
                                            "expected_checksum",
                                            &expected_checksum) &&
        record.object_payload_checksum != expected_checksum) {
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    block_status = mem_service_validate_payload_block(storage_root, &record);
    if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return block_status;
    }
    mem_service_format_record_payload(&record, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_fill_block_ctx(
    const char *payload,
    struct mem_service_block_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    if (!mem_service_payload_get_string(payload,
                                        "request_id",
                                        ctx->request_id,
                                        sizeof(ctx->request_id))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (!mem_service_payload_get_string(payload,
                                        "prefix_group",
                                        ctx->prefix_group,
                                        sizeof(ctx->prefix_group))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (!mem_service_payload_get_string(payload,
                                        "group_id",
                                        ctx->group_id,
                                        sizeof(ctx->group_id))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (!mem_service_payload_get_string(payload,
                                        "block_hash",
                                        ctx->block_hash,
                                        sizeof(ctx->block_hash))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    ctx->placement_node = mem_service_payload_get_u32(payload, "placement_node", 0);
    ctx->placement_level = mem_service_payload_get_u32(payload, "placement_level", 0);
    ctx->hot_segment_id = mem_service_payload_get_u64(payload, "hot_segment_id", 0);
    ctx->result_segment_id = mem_service_payload_get_u64(payload, "result_segment_id", 0);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_publish_kv(struct mem_service *svc,
                                                           const char *payload,
                                                           char *response,
                                                           size_t response_len)
{
    struct mem_service_block_ctx ctx;
    struct mem_service_record record;
    char block_key[96];
    enum mem_service_kvcache_state state =
        mem_service_payload_get_state(payload, MEM_SERVICE_KVCACHE_STATE_FILLED);
    enum mem_service_wire_status parse_status = mem_service_fill_block_ctx(payload, &ctx);

    if (parse_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return parse_status;
    }
    if (mem_service_bootstrap_kvcache(svc, &ctx, &record) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    mem_service_build_block_key_from_hash(ctx.block_hash, block_key, sizeof(block_key));
    if (ctx.result_segment_id != 0 && ctx.result_segment_id > record.last_result_segment) {
        int rc = mem_service_apply_block_result(svc, &ctx, ctx.result_segment_id, state, &record);

        if (rc < 0 || rc == 2) {
            return MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
        }
        if (rc != 0 && mem_service_get_record(svc, block_key, &record) != 0) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
    }
    if (mem_service_get_record(svc, block_key, &record) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (record.state != state) {
        struct mem_service_record *stored = mem_service_find_record(svc, block_key);

        if (stored == NULL) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        stored->state = state;
        stored->version += 1;
        if (mem_service_get_record(svc, block_key, &record) != 0) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
    }
    snprintf(response,
             response_len,
             "status=ok\nkey=%s\nblock_hash=%s\nstate=%s\nversion=%" PRIu64 "\n",
             block_key,
             record.block_hash,
             mem_service_kvcache_state_name(record.state),
             record.version);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_resolve_kv(struct mem_service *svc,
                                                           const char *payload,
                                                           char *response,
                                                           size_t response_len)
{
    struct mem_service_record record;
    char key[sizeof(record.key)];
    char block_hash[sizeof(record.block_hash)];

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        if (!mem_service_payload_get_string(payload,
                                            "block_hash",
                                            block_hash,
                                            sizeof(block_hash))) {
            return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
        }
        mem_service_build_block_key_from_hash(block_hash, key, sizeof(key));
    }
    if (mem_service_get_record(svc, key, &record) != 0 ||
        record.kind != MEM_SERVICE_RECORD_BLOCK_META) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    mem_service_format_record_payload(&record, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_register_prefix(struct mem_service *svc,
                                                                const char *payload,
                                                                char *response,
                                                                size_t response_len)
{
    struct mem_service_block_ctx ctx;
    struct mem_service_record block;
    struct mem_service_record prefix;
    char prefix_key[96];
    enum mem_service_kvcache_state state =
        mem_service_payload_get_state(payload, MEM_SERVICE_KVCACHE_STATE_FILLED);
    enum mem_service_wire_status parse_status = mem_service_fill_block_ctx(payload, &ctx);

    if (parse_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return parse_status;
    }
    if (ctx.result_segment_id == 0) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_bootstrap_kvcache(svc, &ctx, &block) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    mem_service_build_prefix_key_from_parts(ctx.request_id,
                                            ctx.prefix_group,
                                            prefix_key,
                                            sizeof(prefix_key));
    if (mem_service_get_record(svc, prefix_key, &prefix) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (prefix.state != state) {
        struct mem_service_record *stored_prefix = mem_service_find_record(svc, prefix_key);
        struct mem_service_record *stored_block;
        char block_key[96];

        mem_service_build_block_key_from_hash(ctx.block_hash, block_key, sizeof(block_key));
        stored_block = mem_service_find_record(svc, block_key);
        if (stored_prefix == NULL || stored_block == NULL) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        stored_prefix->state = state;
        stored_prefix->version += 1;
        stored_block->state = state;
        stored_block->version += 1;
        if (mem_service_get_record(svc, prefix_key, &prefix) != 0) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
    }
    mem_service_format_record_payload(&prefix, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_lookup_prefix(struct mem_service *svc,
                                                              const char *payload,
                                                              char *response,
                                                              size_t response_len)
{
    struct mem_service_record record;
    char request_id[64];
    char prefix_group[64];
    char key[96];

    if (!mem_service_payload_get_string(payload,
                                        "request_id",
                                        request_id,
                                        sizeof(request_id)) ||
        !mem_service_payload_get_string(payload,
                                        "prefix_group",
                                        prefix_group,
                                        sizeof(prefix_group))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    mem_service_build_prefix_key_from_parts(request_id, prefix_group, key, sizeof(key));
    if (mem_service_get_record(svc, key, &record) != 0 ||
        record.kind != MEM_SERVICE_RECORD_REQUEST_PREFIX) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    mem_service_format_record_payload(&record, response, response_len);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static const char *mem_service_record_kind_name(enum mem_service_record_kind kind)
{
    switch (kind) {
    case MEM_SERVICE_RECORD_PREFIX_GROUP:
        return "prefix_group";
    case MEM_SERVICE_RECORD_REQUEST_PREFIX:
        return "request_prefix";
    case MEM_SERVICE_RECORD_BLOCK_META:
        return "block_meta";
    case MEM_SERVICE_RECORD_WEIGHT_TILE:
        return "weight_tile";
    case MEM_SERVICE_RECORD_KVCACHE_OBJECT:
        return "kvcache_object";
    case MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT:
        return "hidden_range_input";
    case MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT:
        return "hidden_range_output";
    case MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT:
        return "layer_range_placement";
    case MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT:
        return "model_token_result";
    case MEM_SERVICE_RECORD_MODEL_ENGRAM_HISTORY:
        return "model_engram_history";
    case MEM_SERVICE_RECORD_MODEL_ENGRAM_CANDIDATES:
        return "model_engram_candidates";
    case MEM_SERVICE_RECORD_MODEL_ENGRAM_SELECTED:
        return "model_engram_selected";
    case MEM_SERVICE_RECORD_MODEL_ENGRAM_STATE:
        return "model_engram_state";
    case MEM_SERVICE_RECORD_RUNTIME_HANDOFF:
        return "runtime_handoff";
    case MEM_SERVICE_RECORD_EXECUTION_ARTIFACT:
        return "execution_artifact";
    case MEM_SERVICE_RECORD_TRAINING_ARTIFACT:
        return "training_artifact";
    default:
        return "unknown";
    }
}

static size_t mem_service_count_record_kind(const struct mem_service *svc,
                                            enum mem_service_record_kind kind)
{
    size_t count = 0;
    size_t i;

    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (svc->records[i].in_use && svc->records[i].kind == kind) {
            count += 1;
        }
    }
    return count;
}

static enum mem_service_wire_status mem_service_status(struct mem_service *svc,
                                                       char *response,
                                                       size_t response_len)
{
    bool ready = svc->shmem_ready && svc->urma_ready && svc->block_ready;

    snprintf(response,
             response_len,
             "ready=%u\n"
             "shmem_ready=%u\n"
             "urma_ready=%u\n"
             "block_ready=%u\n"
             "record_count=%zu\n"
             "prefix_group_count=%zu\n"
             "prefix_entry_count=%zu\n"
             "kv_segment_count=%zu\n"
             "object_count=%zu\n"
             "runtime_handoff_count=%zu\n"
             "execution_artifact_count=%zu\n"
             "training_artifact_count=%zu\n",
             ready ? 1U : 0U,
             svc->shmem_ready ? 1U : 0U,
             svc->urma_ready ? 1U : 0U,
             svc->block_ready ? 1U : 0U,
             svc->record_count,
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_PREFIX_GROUP),
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_REQUEST_PREFIX),
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_BLOCK_META),
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_KVCACHE_OBJECT),
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_RUNTIME_HANDOFF),
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_EXECUTION_ARTIFACT),
             mem_service_count_record_kind(svc, MEM_SERVICE_RECORD_TRAINING_ARTIFACT));
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_list_records(struct mem_service *svc,
                                                             char *response,
                                                             size_t response_len)
{
    size_t used = 0;
    size_t emitted = 0;
    size_t i;
    int written;

    if (response_len == 0) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    response[0] = '\0';
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *record = &svc->records[i];

        if (!record->in_use) {
            continue;
        }
        written = snprintf(response + used,
                           response_len - used,
                           "record index=%zu kind=%u kind_name=%s key=%s version=%" PRIu64
                           " checksum=%" PRIu64 "\n",
                           i,
                           (uint32_t)record->kind,
                           mem_service_record_kind_name(record->kind),
                           record->key,
                           record->version,
                           record->object_payload_checksum);
        if (written < 0 || (size_t)written >= response_len - used) {
            response[used] = '\0';
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
        used += (size_t)written;
        emitted += 1;
    }
    if (emitted == 0) {
        snprintf(response, response_len, "record_count=0\n");
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static int mem_service_append_snapshot_text(char *response,
                                            size_t response_len,
                                            size_t *used,
                                            const char *fmt,
                                            ...)
{
    va_list ap;
    int written;

    if (response == NULL || used == NULL || *used >= response_len) {
        return -1;
    }
    va_start(ap, fmt);
    written = vsnprintf(response + *used, response_len - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= response_len - *used) {
        response[*used] = '\0';
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static int mem_service_append_snapshot_record_text(
    char *response,
    size_t response_len,
    size_t *used,
    const struct mem_service_record *record)
{
    uint32_t member_index;

    if (mem_service_append_snapshot_text(
            response,
            response_len,
            used,
            "record_begin\n"
            "kind=%u\n"
            "key=%s\n"
            "request_id=%s\n"
            "prefix_group=%s\n"
            "group_id=%s\n"
            "block_hash=%s\n"
            "session_id=%s\n"
            "model_key=%s\n"
            "artifact_kind=%s\n"
            "artifact_id=%s\n"
            "placement_node=%u\n"
            "placement_level=%u\n"
            "hot_segment_id=%" PRIu64 "\n"
            "state=%u\n"
            "version=%" PRIu64 "\n"
            "last_result_segment=%" PRIu64 "\n"
            "object_owner_node=%u\n"
            "object_payload_kind=%u\n"
            "object_backing_offset=%" PRIu64 "\n"
            "object_backing_len=%" PRIu64 "\n"
            "object_payload_checksum=%" PRIu64 "\n"
            "object_publish_monotonic_ms=%" PRIu64 "\n"
            "object_publish_supernode_ms=%" PRIu64 "\n"
            "object_publish_supernode_offset_ms=%" PRId64 "\n"
            "member_count=%u\n",
            (uint32_t)record->kind,
            record->key,
            record->request_id,
            record->prefix_group,
            record->group_id,
            record->block_hash,
            record->session_id,
            record->model_key,
            record->artifact_kind,
            record->artifact_id,
            record->placement_node,
            record->placement_level,
            record->hot_segment_id,
            (uint32_t)record->state,
            record->version,
            record->last_result_segment,
            record->object_owner_node,
            record->object_payload_kind,
            record->object_backing_offset,
            record->object_backing_len,
            record->object_payload_checksum,
            record->object_publish_monotonic_ms,
            record->object_publish_supernode_ms,
            record->object_publish_supernode_offset_ms,
            record->member_count) != 0) {
        return -1;
    }
    for (member_index = 0;
         member_index < record->member_count &&
         member_index < MEM_SERVICE_MAX_GROUP_MEMBERS;
         ++member_index) {
        if (mem_service_append_snapshot_text(response,
                                             response_len,
                                             used,
                                             "member%u=%s\n",
                                             member_index,
                                             record->member_block_hashes[member_index]) != 0) {
            return -1;
        }
    }
    return mem_service_append_snapshot_text(response, response_len, used, "record_end\n");
}

static int mem_service_append_snapshot_idempotency_text(
    char *response,
    size_t response_len,
    size_t *used,
    const struct mem_service_idempotency_record *record)
{
    const char *cursor;
    const char *end;

    if (mem_service_append_snapshot_text(response,
                                         response_len,
                                         used,
                                         "idempotency_begin\n"
                                         "key=%s\n"
                                         "operation=%u\n"
                                         "request_checksum=%u\n"
                                         "status=%u\n"
                                         "response_len=%u\n",
                                         record->key,
                                         record->operation,
                                         record->request_checksum,
                                         record->status,
                                         record->response_len) != 0) {
        return -1;
    }
    cursor = record->response;
    end = record->response + record->response_len;
    while (cursor < end) {
        const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_len = newline != NULL ? (size_t)(newline - cursor)
                                          : (size_t)(end - cursor);

        if (mem_service_append_snapshot_text(response,
                                             response_len,
                                             used,
                                             "response_line=%.*s\n",
                                             (int)line_len,
                                             cursor) != 0) {
            return -1;
        }
        if (newline == NULL) {
            break;
        }
        cursor = newline + 1;
    }
    return mem_service_append_snapshot_text(response,
                                            response_len,
                                            used,
                                            "idempotency_end\n");
}

static int mem_service_append_snapshot_audit_text(
    char *response,
    size_t response_len,
    size_t *used,
    const struct mem_service_audit_event *event)
{
    return mem_service_append_snapshot_text(
        response,
        response_len,
        used,
        "audit_begin\n"
        "sequence=%" PRIu64 "\n"
        "monotonic_ms=%" PRIu64 "\n"
        "operation=%u\n"
        "status=%u\n"
        "request_checksum=%u\n"
        "response_checksum=%u\n"
        "idempotency_replay=%u\n"
        "key=%s\n"
        "session_id=%s\n"
        "model_key=%s\n"
        "artifact_kind=%s\n"
        "artifact_id=%s\n"
        "idempotency_key=%s\n"
        "version=%" PRIu64 "\n"
        "checksum=%" PRIu64 "\n"
        "audit_end\n",
        event->sequence,
        event->monotonic_ms,
        event->operation,
        event->status,
        event->request_checksum,
        event->response_checksum,
        event->idempotency_replay,
        event->key,
        event->session_id,
        event->model_key,
        event->artifact_kind,
        event->artifact_id,
        event->idempotency_key,
        event->version,
        event->checksum);
}

static bool mem_service_has_record_at_or_after(const struct mem_service *svc,
                                               size_t start_index)
{
    size_t i;

    for (i = start_index; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (svc->records[i].in_use) {
            return true;
        }
    }
    return false;
}

static enum mem_service_wire_status mem_service_export_snapshot(struct mem_service *svc,
                                                                char *response,
                                                                size_t response_len)
{
    size_t used = 0;
    size_t i;

    if (response_len == 0) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    response[0] = '\0';
    if (mem_service_append_snapshot_text(response,
                                         response_len,
                                         &used,
                                         "%s\nrecord_count=%zu\n"
                                         "audit_next_sequence=%" PRIu64 "\n"
                                         "audit_event_count=%" PRIu64 "\n",
                                         MEM_SERVICE_STORE_MAGIC,
                                         svc->record_count,
                                         svc->audit_next_sequence,
                                         svc->audit_event_count) != 0) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *record = &svc->records[i];

        if (!record->in_use) {
            continue;
        }
        if (mem_service_append_snapshot_record_text(response, response_len, &used, record) != 0) {
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
    }
    for (i = 0; i < MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS; ++i) {
        const struct mem_service_idempotency_record *record = &svc->idempotency_records[i];

        if (!record->in_use) {
            continue;
        }
        if (mem_service_append_snapshot_idempotency_text(response,
                                                         response_len,
                                                         &used,
                                                         record) != 0) {
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
    }
    if (svc->audit_event_count > 0) {
        uint64_t first_sequence = mem_service_audit_first_sequence(svc);
        uint64_t sequence;

        for (sequence = first_sequence; sequence < svc->audit_next_sequence; ++sequence) {
            const struct mem_service_audit_event *event =
                mem_service_find_audit_sequence(svc, sequence);

            if (event == NULL) {
                continue;
            }
            if (mem_service_append_snapshot_audit_text(response,
                                                       response_len,
                                                       &used,
                                                       event) != 0) {
                return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
            }
        }
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_export_snapshot_page(
    struct mem_service *svc,
    const char *payload,
    char *response,
    size_t response_len)
{
    char records[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    size_t records_len;
    size_t record_used = 0;
    size_t start_index;
    size_t next_index;
    size_t emitted = 0;
    size_t i;
    uint64_t requested_start;
    uint64_t max_records;
    bool complete = true;
    bool stopped_on_capacity = false;

    if (response_len <= MEM_SERVICE_SNAPSHOT_PAGE_HEADER_RESERVE) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    requested_start = mem_service_payload_get_u64(payload, "start_index", 0);
    if (requested_start >= MEM_SERVICE_MAX_RECORDS) {
        requested_start = MEM_SERVICE_MAX_RECORDS;
    }
    start_index = (size_t)requested_start;
    next_index = start_index;
    max_records = mem_service_payload_get_u64(payload, "max_records", 0);
    records_len = response_len - MEM_SERVICE_SNAPSHOT_PAGE_HEADER_RESERVE;
    records[0] = '\0';

    for (i = start_index; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *record = &svc->records[i];
        size_t before;

        if (!record->in_use) {
            continue;
        }
        if (max_records != 0 && emitted >= max_records) {
            complete = !mem_service_has_record_at_or_after(svc, i);
            next_index = complete ? MEM_SERVICE_MAX_RECORDS : i;
            break;
        }
        before = record_used;
        if (mem_service_append_snapshot_record_text(records,
                                                    records_len,
                                                    &record_used,
                                                    record) != 0) {
            if (emitted == 0) {
                return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
            }
            records[before] = '\0';
            record_used = before;
            complete = false;
            stopped_on_capacity = true;
            next_index = i;
            break;
        }
        emitted += 1;
        next_index = i + 1U;
    }
    if (!stopped_on_capacity && next_index < MEM_SERVICE_MAX_RECORDS) {
        complete = !mem_service_has_record_at_or_after(svc, next_index);
        if (complete) {
            next_index = MEM_SERVICE_MAX_RECORDS;
        }
    }
    snprintf(response,
             response_len,
             "snapshot_page=1\n"
             "store_magic=%s\n"
             "record_count=%zu\n"
             "start_index=%zu\n"
             "next_index=%zu\n"
             "records_emitted=%zu\n"
             "complete=%u\n"
             "%s",
             MEM_SERVICE_STORE_MAGIC,
             svc->record_count,
             start_index,
             next_index,
             emitted,
             complete ? 1U : 0U,
             records);
    if (strlen(response) >= response_len - 1U) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_restore_snapshot(struct mem_service *svc,
                                                                 const char *payload,
                                                                 char *response,
                                                                 size_t response_len)
{
    struct mem_service restored;

    if (payload == NULL || payload[0] == '\0' ||
        mem_service_init(&restored,
                         svc->shmem_ready,
                         svc->urma_ready,
                         svc->block_ready) != 0 ||
        mem_service_import_snapshot_text(&restored, payload) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    memset(svc->records, 0, sizeof(svc->records));
    memcpy(svc->records, restored.records, sizeof(svc->records));
    memset(svc->idempotency_records, 0, sizeof(svc->idempotency_records));
    memcpy(svc->idempotency_records,
           restored.idempotency_records,
           sizeof(svc->idempotency_records));
    memset(svc->audit_events, 0, sizeof(svc->audit_events));
    memcpy(svc->audit_events, restored.audit_events, sizeof(svc->audit_events));
    svc->record_count = restored.record_count;
    svc->audit_next_sequence = restored.audit_next_sequence;
    svc->audit_event_count = restored.audit_event_count;
    snprintf(response,
             response_len,
             "status=ok\nrestored=1\nrecord_count=%zu\n",
             svc->record_count);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static void mem_service_restore_snapshot_stage_reset(void)
{
    memset(&mem_service_restore_snapshot_stage,
           0,
           sizeof(mem_service_restore_snapshot_stage));
}

static enum mem_service_wire_status mem_service_restore_snapshot_page_begin(
    struct mem_service *svc,
    const char *payload,
    char *response,
    size_t response_len)
{
    uint64_t expected_records = 0;

    mem_service_restore_snapshot_stage_reset();
    if (mem_service_init(&mem_service_restore_snapshot_stage.svc,
                         svc->shmem_ready,
                         svc->urma_ready,
                         svc->block_ready) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    mem_service_restore_snapshot_stage.active = true;
    mem_service_restore_snapshot_stage.next_page_index = 0;
    if (mem_service_payload_get_u64_checked(payload,
                                            "expected_records",
                                            &expected_records)) {
        mem_service_restore_snapshot_stage.has_expected_records = true;
        mem_service_restore_snapshot_stage.expected_records = expected_records;
        if (expected_records == 0) {
            mem_service_restore_snapshot_stage.saw_complete = true;
        }
    }
    snprintf(response,
             response_len,
             "status=ok\nrestore_stage=begun\nexpected_records=%" PRIu64 "\n",
             expected_records);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_restore_snapshot_page_append(
    const char *payload,
    char *response,
    size_t response_len)
{
    uint64_t page_index;
    size_t records_imported = 0;

    if (!mem_service_restore_snapshot_stage.active ||
        !mem_service_payload_get_u64_checked(payload, "page_index", &page_index)) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (page_index != mem_service_restore_snapshot_stage.next_page_index) {
        return MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
    }
    if (mem_service_import_snapshot_records_text(
            &mem_service_restore_snapshot_stage.svc,
            payload,
            &records_imported) != 0) {
        mem_service_restore_snapshot_stage_reset();
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    mem_service_restore_snapshot_stage.next_page_index += 1U;
    if (mem_service_payload_get_u32(payload, "complete", 0) != 0) {
        mem_service_restore_snapshot_stage.saw_complete = true;
    }
    if (mem_service_restore_snapshot_stage.has_expected_records &&
        mem_service_restore_snapshot_stage.svc.record_count >
            mem_service_restore_snapshot_stage.expected_records) {
        mem_service_restore_snapshot_stage_reset();
        return MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
    }
    snprintf(response,
             response_len,
             "status=ok\nrestore_stage=appended\npage_index=%" PRIu64
             "\nrecords_imported=%zu\nrecord_count=%zu\ncomplete=%u\n",
             page_index,
             records_imported,
             mem_service_restore_snapshot_stage.svc.record_count,
             mem_service_restore_snapshot_stage.saw_complete ? 1U : 0U);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_restore_snapshot_page_commit(
    struct mem_service *svc,
    char *response,
    size_t response_len)
{
    if (!mem_service_restore_snapshot_stage.active) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_restore_snapshot_stage.has_expected_records &&
        mem_service_restore_snapshot_stage.svc.record_count !=
            mem_service_restore_snapshot_stage.expected_records) {
        return MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
    }
    if (!mem_service_restore_snapshot_stage.has_expected_records &&
        !mem_service_restore_snapshot_stage.saw_complete) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    memset(svc->records, 0, sizeof(svc->records));
    memcpy(svc->records,
           mem_service_restore_snapshot_stage.svc.records,
           sizeof(svc->records));
    memset(svc->idempotency_records, 0, sizeof(svc->idempotency_records));
    memset(svc->audit_events, 0, sizeof(svc->audit_events));
    svc->record_count = mem_service_restore_snapshot_stage.svc.record_count;
    svc->audit_next_sequence = 1U;
    svc->audit_event_count = 0;
    snprintf(response,
             response_len,
             "status=ok\nrestored=1\nrecord_count=%zu\n",
             svc->record_count);
    mem_service_restore_snapshot_stage_reset();
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_restore_snapshot_page(
    struct mem_service *svc,
    const char *payload,
    char *response,
    size_t response_len)
{
    char action[32];

    if (!mem_service_payload_get_string(payload, "action", action, sizeof(action))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (strcmp(action, "begin") == 0) {
        return mem_service_restore_snapshot_page_begin(svc,
                                                       payload,
                                                       response,
                                                       response_len);
    }
    if (strcmp(action, "append") == 0) {
        return mem_service_restore_snapshot_page_append(payload, response, response_len);
    }
    if (strcmp(action, "commit") == 0) {
        return mem_service_restore_snapshot_page_commit(svc, response, response_len);
    }
    if (strcmp(action, "cancel") == 0) {
        mem_service_restore_snapshot_stage_reset();
        snprintf(response, response_len, "status=ok\nrestore_stage=cancelled\n");
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
}

static enum mem_service_wire_status mem_service_metrics(struct mem_service *svc,
                                                        char *response,
                                                        size_t response_len)
{
    const struct mem_service_metrics *m = &svc->metrics;

    snprintf(response,
             response_len,
             "request_count=%" PRIu64 "\n"
             "ok_count=%" PRIu64 "\n"
             "error_count=%" PRIu64 "\n"
             "not_found_count=%" PRIu64 "\n"
             "stale_ref_count=%" PRIu64 "\n"
             "checksum_mismatch_count=%" PRIu64 "\n"
             "version_conflict_count=%" PRIu64 "\n"
             "invalid_model_binding_count=%" PRIu64 "\n"
             "invalid_session_count=%" PRIu64 "\n"
             "timeout_count=%" PRIu64 "\n"
             "capacity_exceeded_count=%" PRIu64 "\n"
             "unsupported_count=%" PRIu64 "\n"
             "internal_count=%" PRIu64 "\n"
             "fail_closed_count=%" PRIu64 "\n"
             "health_count=%" PRIu64 "\n"
             "ready_count=%" PRIu64 "\n"
             "status_count=%" PRIu64 "\n"
             "list_records_count=%" PRIu64 "\n"
             "metrics_count=%" PRIu64 "\n"
             "audit_log_count=%" PRIu64 "\n"
             "export_snapshot_count=%" PRIu64 "\n"
             "export_snapshot_page_count=%" PRIu64 "\n"
             "restore_snapshot_count=%" PRIu64 "\n"
             "restore_snapshot_page_count=%" PRIu64 "\n"
             "put_object_count=%" PRIu64 "\n"
             "get_object_count=%" PRIu64 "\n"
             "inspect_object_count=%" PRIu64 "\n"
             "get_object_hit_count=%" PRIu64 "\n"
             "get_object_miss_count=%" PRIu64 "\n"
             "register_prefix_count=%" PRIu64 "\n"
             "lookup_prefix_count=%" PRIu64 "\n"
             "prefix_lookup_hit_count=%" PRIu64 "\n"
             "prefix_lookup_miss_count=%" PRIu64 "\n"
             "publish_kv_count=%" PRIu64 "\n"
             "resolve_kv_count=%" PRIu64 "\n"
             "kv_resolve_hit_count=%" PRIu64 "\n"
             "kv_resolve_miss_count=%" PRIu64 "\n"
             "publish_runtime_handoff_count=%" PRIu64 "\n"
             "resolve_runtime_handoff_count=%" PRIu64 "\n"
             "register_execution_artifact_count=%" PRIu64 "\n"
             "query_execution_artifact_count=%" PRIu64 "\n"
             "register_training_artifact_count=%" PRIu64 "\n"
             "query_training_artifact_count=%" PRIu64 "\n"
             "artifact_query_hit_count=%" PRIu64 "\n"
             "artifact_query_miss_count=%" PRIu64 "\n"
             "idempotency_replay_count=%" PRIu64 "\n"
             "idempotency_conflict_count=%" PRIu64 "\n"
             "request_latency_total_ms=%" PRIu64 "\n"
             "request_latency_max_ms=%" PRIu64 "\n"
             "request_latency_le_1ms_count=%" PRIu64 "\n"
             "request_latency_le_5ms_count=%" PRIu64 "\n"
             "request_latency_le_10ms_count=%" PRIu64 "\n"
             "request_latency_le_50ms_count=%" PRIu64 "\n"
             "request_latency_le_100ms_count=%" PRIu64 "\n"
             "request_latency_gt_100ms_count=%" PRIu64 "\n",
             m->request_count,
             m->ok_count,
             m->error_count,
             m->not_found_count,
             m->stale_ref_count,
             m->checksum_mismatch_count,
             m->version_conflict_count,
             m->invalid_model_binding_count,
             m->invalid_session_count,
             m->timeout_count,
             m->capacity_exceeded_count,
             m->unsupported_count,
             m->internal_count,
             m->fail_closed_count,
             m->health_count,
             m->ready_count,
             m->status_count,
             m->list_records_count,
             m->metrics_count,
             m->audit_log_count,
             m->export_snapshot_count,
             m->export_snapshot_page_count,
             m->restore_snapshot_count,
             m->restore_snapshot_page_count,
             m->put_object_count,
             m->get_object_count,
             m->inspect_object_count,
             m->get_object_hit_count,
             m->get_object_miss_count,
             m->register_prefix_count,
             m->lookup_prefix_count,
             m->prefix_lookup_hit_count,
             m->prefix_lookup_miss_count,
             m->publish_kv_count,
             m->resolve_kv_count,
             m->kv_resolve_hit_count,
             m->kv_resolve_miss_count,
             m->publish_runtime_handoff_count,
             m->resolve_runtime_handoff_count,
             m->register_execution_artifact_count,
             m->query_execution_artifact_count,
             m->register_training_artifact_count,
             m->query_training_artifact_count,
             m->artifact_query_hit_count,
             m->artifact_query_miss_count,
             m->idempotency_replay_count,
             m->idempotency_conflict_count,
             m->request_latency_total_ms,
             m->request_latency_max_ms,
             m->request_latency_bucket_counts[0],
             m->request_latency_bucket_counts[1],
             m->request_latency_bucket_counts[2],
             m->request_latency_bucket_counts[3],
             m->request_latency_bucket_counts[4],
             m->request_latency_bucket_counts[5]);
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static const char *mem_service_operation_name(enum mem_service_wire_operation operation)
{
    const struct mem_service_wire_operation_schema *schema =
        mem_service_wire_schema_for_operation(operation);

    return schema != NULL ? schema->name : "unknown";
}

static uint64_t mem_service_audit_first_sequence(const struct mem_service *svc)
{
    if (svc == NULL || svc->audit_event_count == 0 ||
        svc->audit_next_sequence == 0) {
        return 1U;
    }
    if (svc->audit_next_sequence <= svc->audit_event_count) {
        return 1U;
    }
    return svc->audit_next_sequence - svc->audit_event_count;
}

static enum mem_service_wire_status mem_service_audit_log(struct mem_service *svc,
                                                          const char *payload,
                                                          char *response,
                                                          size_t response_len)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);
    uint64_t start_sequence =
        mem_service_wire_payload_get_u64(&view, "start_sequence", 0);
    uint64_t max_events = mem_service_wire_payload_get_u64(&view, "max_events", 16);
    uint64_t first_sequence = mem_service_audit_first_sequence(svc);
    uint64_t sequence;
    uint64_t next_sequence;
    uint64_t emitted = 0;
    size_t used = 0;

    if (max_events == 0) {
        max_events = 16;
    }
    if (max_events > 32) {
        max_events = 32;
    }
    if (start_sequence == 0 || start_sequence < first_sequence) {
        start_sequence = first_sequence;
    }
    next_sequence = start_sequence;
    if (mem_service_append_snapshot_text(response,
                                         response_len,
                                         &used,
                                         "audit_log=1\n"
                                         "retained_events=%" PRIu64 "\n"
                                         "first_sequence=%" PRIu64 "\n"
                                         "start_sequence=%" PRIu64 "\n",
                                         svc->audit_event_count,
                                         first_sequence,
                                         start_sequence) != 0) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    for (sequence = start_sequence;
         sequence < svc->audit_next_sequence && emitted < max_events;
         ++sequence) {
        const struct mem_service_audit_event *event =
            mem_service_find_audit_sequence(svc, sequence);

        if (event == NULL) {
            continue;
        }
        if (mem_service_append_snapshot_text(
                response,
                response_len,
                &used,
                "audit_begin\n"
                "sequence=%" PRIu64 "\n"
                "monotonic_ms=%" PRIu64 "\n"
                "operation=%u\n"
                "operation_name=%s\n"
                "status=%u\n"
                "status_name=%s\n"
                "request_checksum=%u\n"
                "response_checksum=%u\n"
                "idempotency_replay=%u\n"
                "key=%s\n"
                "session_id=%s\n"
                "model_key=%s\n"
                "artifact_kind=%s\n"
                "artifact_id=%s\n"
                "idempotency_key=%s\n"
                "version=%" PRIu64 "\n"
                "checksum=%" PRIu64 "\n"
                "audit_end\n",
                event->sequence,
                event->monotonic_ms,
                event->operation,
                mem_service_operation_name(
                    (enum mem_service_wire_operation)event->operation),
                event->status,
                mem_service_wire_status_name(
                    (enum mem_service_wire_status)event->status),
                event->request_checksum,
                event->response_checksum,
                event->idempotency_replay,
                event->key,
                event->session_id,
                event->model_key,
                event->artifact_kind,
                event->artifact_id,
                event->idempotency_key,
                event->version,
                event->checksum) != 0) {
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
        emitted += 1U;
        next_sequence = sequence + 1U;
    }
    if (mem_service_append_snapshot_text(response,
                                         response_len,
                                         &used,
                                         "events_emitted=%" PRIu64 "\n"
                                         "next_sequence=%" PRIu64 "\n"
                                         "complete=%u\n",
                                         emitted,
                                         next_sequence,
                                         next_sequence >= svc->audit_next_sequence
                                             ? 1U
                                             : 0U) != 0) {
        return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_dispatch_operation(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    const char *storage_root)
{
    response[0] = '\0';
    switch (operation) {
    case MEM_SERVICE_WIRE_OP_HEALTH:
        snprintf(response, response_len, "ok");
        return MEM_SERVICE_WIRE_STATUS_OK;
    case MEM_SERVICE_WIRE_OP_READY:
        if (svc->shmem_ready && svc->urma_ready && svc->block_ready) {
            snprintf(response, response_len, "ok");
            return MEM_SERVICE_WIRE_STATUS_OK;
        }
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    case MEM_SERVICE_WIRE_OP_STATUS:
        return mem_service_status(svc, response, response_len);
    case MEM_SERVICE_WIRE_OP_LIST_RECORDS:
        return mem_service_list_records(svc, response, response_len);
    case MEM_SERVICE_WIRE_OP_METRICS:
        return mem_service_metrics(svc, response, response_len);
    case MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT:
        return mem_service_export_snapshot(svc, response, response_len);
    case MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE:
        return mem_service_export_snapshot_page(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT:
        return mem_service_restore_snapshot(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE:
        return mem_service_restore_snapshot_page(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_AUDIT_LOG:
        return mem_service_audit_log(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_PUT_OBJECT:
        return mem_service_put_object(svc,
                                      payload,
                                      response,
                                      response_len,
                                      storage_root);
    case MEM_SERVICE_WIRE_OP_GET_OBJECT:
        return mem_service_get_object(svc,
                                      payload,
                                      response,
                                      response_len,
                                      storage_root);
    case MEM_SERVICE_WIRE_OP_INSPECT_OBJECT:
        return mem_service_inspect_object(svc,
                                          payload,
                                          response,
                                          response_len,
                                          storage_root);
    case MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY:
        return mem_service_register_prefix(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY:
        return mem_service_lookup_prefix(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT:
        return mem_service_publish_kv(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT:
        return mem_service_resolve_kv(svc, payload, response, response_len);
    case MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF:
        return mem_service_store_artifact(svc,
                                          payload,
                                          MEM_SERVICE_RECORD_RUNTIME_HANDOFF,
                                          response,
                                          response_len,
                                          storage_root);
    case MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF:
        return mem_service_query_artifact(svc,
                                          payload,
                                          MEM_SERVICE_RECORD_RUNTIME_HANDOFF,
                                          response,
                                          response_len,
                                          storage_root);
    case MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT:
        return mem_service_store_artifact(svc,
                                          payload,
                                          MEM_SERVICE_RECORD_EXECUTION_ARTIFACT,
                                          response,
                                          response_len,
                                          storage_root);
    case MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT:
        return mem_service_query_artifact(svc,
                                          payload,
                                          MEM_SERVICE_RECORD_EXECUTION_ARTIFACT,
                                          response,
                                          response_len,
                                          storage_root);
    case MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT:
        return mem_service_store_artifact(svc,
                                          payload,
                                          MEM_SERVICE_RECORD_TRAINING_ARTIFACT,
                                          response,
                                          response_len,
                                          storage_root);
    case MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT:
        return mem_service_query_artifact(svc,
                                          payload,
                                          MEM_SERVICE_RECORD_TRAINING_ARTIFACT,
                                          response,
                                          response_len,
                                          storage_root);
    default:
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
}

static bool mem_service_status_is_fail_closed(enum mem_service_wire_status status)
{
    return status == MEM_SERVICE_WIRE_STATUS_STALE_REF ||
           status == MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH ||
           status == MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT ||
           status == MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING ||
           status == MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
}

static void mem_service_record_status_metrics(struct mem_service_metrics *metrics,
                                              enum mem_service_wire_status status)
{
    if (status == MEM_SERVICE_WIRE_STATUS_OK) {
        metrics->ok_count += 1U;
        return;
    }

    metrics->error_count += 1U;
    switch (status) {
    case MEM_SERVICE_WIRE_STATUS_NOT_FOUND:
        metrics->not_found_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_STALE_REF:
        metrics->stale_ref_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH:
        metrics->checksum_mismatch_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT:
        metrics->version_conflict_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING:
        metrics->invalid_model_binding_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_INVALID_SESSION:
        metrics->invalid_session_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_TIMEOUT:
        metrics->timeout_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED:
        metrics->capacity_exceeded_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_UNSUPPORTED:
        metrics->unsupported_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_INTERNAL:
        metrics->internal_count += 1U;
        break;
    case MEM_SERVICE_WIRE_STATUS_OK:
    default:
        break;
    }
    if (mem_service_status_is_fail_closed(status)) {
        metrics->fail_closed_count += 1U;
    }
}

static void mem_service_record_latency_metrics(struct mem_service_metrics *metrics,
                                               uint64_t latency_ms)
{
    size_t bucket_index;

    metrics->request_latency_total_ms += latency_ms;
    if (latency_ms > metrics->request_latency_max_ms) {
        metrics->request_latency_max_ms = latency_ms;
    }
    for (bucket_index = 0;
         bucket_index < MEM_SERVICE_METRIC_LATENCY_BUCKET_COUNT;
         ++bucket_index) {
        if (latency_ms <= mem_service_latency_bucket_limits_ms[bucket_index]) {
            metrics->request_latency_bucket_counts[bucket_index] += 1U;
            return;
        }
    }
    metrics->request_latency_bucket_counts[MEM_SERVICE_METRIC_LATENCY_BUCKET_COUNT - 1U] +=
        1U;
}

static void mem_service_record_operation_metrics(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    enum mem_service_wire_status status,
    uint64_t latency_ms)
{
    struct mem_service_metrics *metrics = &svc->metrics;
    bool hit = status == MEM_SERVICE_WIRE_STATUS_OK;

    metrics->request_count += 1U;
    mem_service_record_status_metrics(metrics, status);
    mem_service_record_latency_metrics(metrics, latency_ms);

    switch (operation) {
    case MEM_SERVICE_WIRE_OP_HEALTH:
        metrics->health_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_READY:
        metrics->ready_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_STATUS:
        metrics->status_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_LIST_RECORDS:
        metrics->list_records_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_METRICS:
        metrics->metrics_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_AUDIT_LOG:
        metrics->audit_log_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT:
        metrics->export_snapshot_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE:
        metrics->export_snapshot_page_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT:
        metrics->restore_snapshot_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE:
        metrics->restore_snapshot_page_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_PUT_OBJECT:
        metrics->put_object_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_GET_OBJECT:
        metrics->get_object_count += 1U;
        if (hit) {
            metrics->get_object_hit_count += 1U;
        } else if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            metrics->get_object_miss_count += 1U;
        }
        break;
    case MEM_SERVICE_WIRE_OP_INSPECT_OBJECT:
        metrics->inspect_object_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY:
        metrics->register_prefix_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY:
        metrics->lookup_prefix_count += 1U;
        if (hit) {
            metrics->prefix_lookup_hit_count += 1U;
        } else if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            metrics->prefix_lookup_miss_count += 1U;
        }
        break;
    case MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT:
        metrics->publish_kv_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT:
        metrics->resolve_kv_count += 1U;
        if (hit) {
            metrics->kv_resolve_hit_count += 1U;
        } else if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            metrics->kv_resolve_miss_count += 1U;
        }
        break;
    case MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF:
        metrics->publish_runtime_handoff_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF:
        metrics->resolve_runtime_handoff_count += 1U;
        if (hit) {
            metrics->artifact_query_hit_count += 1U;
        } else if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            metrics->artifact_query_miss_count += 1U;
        }
        break;
    case MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT:
        metrics->register_execution_artifact_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT:
        metrics->query_execution_artifact_count += 1U;
        if (hit) {
            metrics->artifact_query_hit_count += 1U;
        } else if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            metrics->artifact_query_miss_count += 1U;
        }
        break;
    case MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT:
        metrics->register_training_artifact_count += 1U;
        break;
    case MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT:
        metrics->query_training_artifact_count += 1U;
        if (hit) {
            metrics->artifact_query_hit_count += 1U;
        } else if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            metrics->artifact_query_miss_count += 1U;
        }
        break;
    default:
        break;
    }
}

static struct mem_service_idempotency_record *mem_service_find_idempotency_record(
    struct mem_service *svc,
    const char *key)
{
    size_t i;

    if (svc == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS; ++i) {
        struct mem_service_idempotency_record *record =
            &svc->idempotency_records[i];

        if (record->in_use && strcmp(record->key, key) == 0) {
            return record;
        }
    }
    return NULL;
}

static struct mem_service_idempotency_record *mem_service_alloc_idempotency_record(
    struct mem_service *svc)
{
    size_t i;

    if (svc == NULL) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS; ++i) {
        struct mem_service_idempotency_record *record =
            &svc->idempotency_records[i];

        if (!record->in_use) {
            return record;
        }
    }
    return NULL;
}

static bool mem_service_payload_get_idempotency_key(const char *payload,
                                                    char *key,
                                                    size_t key_len)
{
    if (key == NULL || key_len == 0) {
        return false;
    }
    key[0] = '\0';
    return mem_service_payload_get_header_string(payload,
                                                 "idempotency_key",
                                                 key,
                                                 key_len);
}

static uint32_t mem_service_idempotency_request_checksum(const char *payload)
{
    size_t payload_len = payload != NULL ? strlen(payload) : 0;

    return mem_service_wire_checksum(payload != NULL ? payload : "", payload_len);
}

static enum mem_service_wire_status mem_service_try_idempotency_replay(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    struct mem_service_idempotency_record **pending_record_out,
    bool *handled_out)
{
    char key[MEM_SERVICE_IDEMPOTENCY_KEY_LEN];
    uint32_t request_checksum;
    struct mem_service_idempotency_record *record;

    if (pending_record_out != NULL) {
        *pending_record_out = NULL;
    }
    if (handled_out != NULL) {
        *handled_out = false;
    }
    if (!mem_service_operation_mutates(operation, payload) ||
        !mem_service_payload_get_idempotency_key(payload, key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }

    request_checksum = mem_service_idempotency_request_checksum(payload);
    record = mem_service_find_idempotency_record(svc, key);
    if (record == NULL) {
        record = mem_service_alloc_idempotency_record(svc);
        if (record == NULL) {
            if (handled_out != NULL) {
                *handled_out = true;
            }
            snprintf(response,
                     response_len,
                     "status=capacity_exceeded\nidempotency_key=%s\n",
                     key);
            return MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
        if (pending_record_out != NULL) {
            *pending_record_out = record;
        }
        return MEM_SERVICE_WIRE_STATUS_OK;
    }

    if (handled_out != NULL) {
        *handled_out = true;
    }
    if (record->operation != (uint32_t)operation ||
        record->request_checksum != request_checksum) {
        svc->metrics.idempotency_conflict_count += 1U;
        snprintf(response,
                 response_len,
                 "status=version_conflict\nidempotency_key=%s\n",
                 key);
        return MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
    }

    svc->metrics.idempotency_replay_count += 1U;
    if (response != NULL && response_len > 0) {
        size_t copy_len = record->response_len;

        if (copy_len >= response_len) {
            copy_len = response_len - 1U;
        }
        memcpy(response, record->response, copy_len);
        response[copy_len] = '\0';
    }
    return (enum mem_service_wire_status)record->status;
}

static void mem_service_complete_idempotency_record(
    struct mem_service_idempotency_record *record,
    enum mem_service_wire_operation operation,
    const char *payload,
    enum mem_service_wire_status status,
    const char *response)
{
    char key[MEM_SERVICE_IDEMPOTENCY_KEY_LEN];
    size_t response_len = response != NULL ? strlen(response) : 0;

    if (record == NULL ||
        !mem_service_payload_get_idempotency_key(payload, key, sizeof(key))) {
        return;
    }
    memset(record, 0, sizeof(*record));
    record->in_use = true;
    snprintf(record->key, sizeof(record->key), "%s", key);
    record->operation = (uint32_t)operation;
    record->request_checksum = mem_service_idempotency_request_checksum(payload);
    record->status = (uint32_t)status;
    if (response_len >= sizeof(record->response)) {
        response_len = sizeof(record->response) - 1U;
    }
    if (response_len > 0) {
        memcpy(record->response, response, response_len);
    }
    record->response[response_len] = '\0';
    record->response_len = (uint32_t)response_len;
}

static void mem_service_payload_get_audit_string(const char *payload,
                                                 const char *primary_name,
                                                 const char *fallback_name,
                                                 char *out,
                                                 size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (payload == NULL) {
        return;
    }
    if (primary_name != NULL &&
        mem_service_payload_get_header_string(payload, primary_name, out, out_len)) {
        return;
    }
    if (fallback_name != NULL) {
        (void)mem_service_payload_get_header_string(payload,
                                                    fallback_name,
                                                    out,
                                                    out_len);
    }
}

static uint64_t mem_service_payload_get_audit_u64(const char *payload,
                                                  const char *primary_name,
                                                  const char *fallback_name)
{
    char value[64];

    value[0] = '\0';
    if (payload == NULL) {
        return 0;
    }
    if (primary_name != NULL &&
        mem_service_payload_get_header_string(payload,
                                              primary_name,
                                              value,
                                              sizeof(value))) {
        return mem_service_parse_u64_value(value, 0);
    }
    if (fallback_name != NULL &&
        mem_service_payload_get_header_string(payload,
                                              fallback_name,
                                              value,
                                              sizeof(value))) {
        return mem_service_parse_u64_value(value, 0);
    }
    return 0;
}

static bool mem_service_should_audit_operation(enum mem_service_wire_operation operation,
                                               const char *payload,
                                               enum mem_service_wire_status status)
{
    return mem_service_operation_mutates(operation, payload) ||
           mem_service_status_is_fail_closed(status);
}

static bool mem_service_append_audit_event(struct mem_service *svc,
                                           enum mem_service_wire_operation operation,
                                           const char *payload,
                                           enum mem_service_wire_status status,
                                           const char *response,
                                           bool idempotency_replay,
                                           const struct mem_service_audit_event **event_out)
{
    struct mem_service_audit_event *event;
    uint64_t sequence;
    size_t slot_index;

    if (event_out != NULL) {
        *event_out = NULL;
    }
    if (svc == NULL ||
        !mem_service_should_audit_operation(operation, payload, status)) {
        return false;
    }
    if (svc->audit_next_sequence == 0) {
        svc->audit_next_sequence = 1U;
    }
    sequence = svc->audit_next_sequence;
    slot_index = (size_t)((sequence - 1U) % MEM_SERVICE_MAX_AUDIT_EVENTS);
    event = &svc->audit_events[slot_index];
    memset(event, 0, sizeof(*event));
    event->in_use = true;
    event->sequence = sequence;
    event->monotonic_ms = mem_service_monotonic_ms();
    event->operation = (uint32_t)operation;
    event->status = (uint32_t)status;
    event->request_checksum = mem_service_idempotency_request_checksum(payload);
    event->response_checksum =
        mem_service_wire_checksum(response != NULL ? response : "",
                                  response != NULL ? strlen(response) : 0);
    event->idempotency_replay = idempotency_replay ? 1U : 0U;
    mem_service_payload_get_audit_string(payload,
                                         "key",
                                         NULL,
                                         event->key,
                                         sizeof(event->key));
    mem_service_payload_get_audit_string(payload,
                                         "session_id",
                                         "expected_session_id",
                                         event->session_id,
                                         sizeof(event->session_id));
    mem_service_payload_get_audit_string(payload,
                                         "model_key",
                                         "expected_model_key",
                                         event->model_key,
                                         sizeof(event->model_key));
    mem_service_payload_get_audit_string(payload,
                                         "artifact_kind",
                                         "expected_artifact_kind",
                                         event->artifact_kind,
                                         sizeof(event->artifact_kind));
    mem_service_payload_get_audit_string(payload,
                                         "artifact_id",
                                         "expected_artifact_id",
                                         event->artifact_id,
                                         sizeof(event->artifact_id));
    mem_service_payload_get_audit_string(payload,
                                         "idempotency_key",
                                         NULL,
                                         event->idempotency_key,
                                         sizeof(event->idempotency_key));
    event->version = mem_service_payload_get_audit_u64(payload,
                                                       "version",
                                                       "expected_version");
    event->checksum = mem_service_payload_get_audit_u64(payload,
                                                        "checksum",
                                                        "expected_checksum");
    svc->audit_next_sequence += 1U;
    if (svc->audit_event_count < MEM_SERVICE_MAX_AUDIT_EVENTS) {
        svc->audit_event_count += 1U;
    }
    if (event_out != NULL) {
        *event_out = event;
    }
    return true;
}

static enum mem_service_wire_status mem_service_handle_operation(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    const char *store_path,
    const char *storage_root)
{
    uint64_t start_ms = mem_service_monotonic_ms();
    uint64_t end_ms;
    uint64_t latency_ms;
    struct mem_service_idempotency_record *pending_idempotency = NULL;
    const struct mem_service_audit_event *audit_event = NULL;
    bool idempotency_handled = false;
    bool audit_appended = false;
    enum mem_service_wire_status status =
        mem_service_try_idempotency_replay(svc,
                                           operation,
                                           payload,
                                           response,
                                           response_len,
                                           &pending_idempotency,
                                           &idempotency_handled);

    if (!idempotency_handled) {
        status = mem_service_dispatch_operation(svc,
                                                operation,
                                                payload,
                                                response,
                                                response_len,
                                                storage_root);
        mem_service_complete_idempotency_record(pending_idempotency,
                                                operation,
                                                payload,
                                                status,
                                                response);
    }

    audit_appended = mem_service_append_audit_event(svc,
                                                    operation,
                                                    payload,
                                                    status,
                                                    response,
                                                    idempotency_handled,
                                                    &audit_event);
    if (audit_appended && store_path != NULL &&
        (mem_service_append_journal(store_path,
                                    pending_idempotency,
                                    audit_event) != 0 ||
         mem_service_save_store(svc, store_path) != 0)) {
        if (status == MEM_SERVICE_WIRE_STATUS_OK &&
            mem_service_operation_mutates(operation, payload)) {
            const struct mem_service_audit_event *failure_audit_event = NULL;

            status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
            snprintf(response, response_len, "durable_store_save_failed\n");
            mem_service_complete_idempotency_record(pending_idempotency,
                                                    operation,
                                                    payload,
                                                    status,
                                                    response);
            (void)mem_service_append_audit_event(svc,
                                                 operation,
                                                 payload,
                                                 status,
                                                 response,
                                                 idempotency_handled,
                                                 &failure_audit_event);
            (void)mem_service_append_journal(store_path,
                                             pending_idempotency,
                                             failure_audit_event);
            (void)mem_service_save_store(svc, store_path);
        }
    }

    end_ms = mem_service_monotonic_ms();
    latency_ms = end_ms >= start_ms ? end_ms - start_ms : 0;
    mem_service_record_operation_metrics(svc, operation, status, latency_ms);
    return status;
}

static bool mem_service_operation_mutates(enum mem_service_wire_operation operation,
                                          const char *payload)
{
    switch (operation) {
    case MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT:
        return true;
    case MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE: {
        char action[32];

        return mem_service_payload_get_string(payload, "action", action, sizeof(action)) &&
               strcmp(action, "commit") == 0;
    }
    case MEM_SERVICE_WIRE_OP_PUT_OBJECT:
    case MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY:
    case MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT:
    case MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF:
    case MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT:
    case MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT:
        return true;
    default:
        return false;
    }
}

static int mem_service_handle_client(int client_fd,
                                     struct mem_service *svc,
                                     const char *store_path,
                                     const char *storage_root)
{
    struct mem_service_wire_header request;
    enum mem_service_wire_status status;
    uint8_t request_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN + 1];
    char response_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];

    if (mem_service_read_full(client_fd, &request, sizeof(request)) != 0) {
        return -1;
    }
    if (!mem_service_wire_header_is_compatible(&request)) {
        return -1;
    }
    if (request.payload_len > MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        return mem_service_send_response(client_fd,
                                         &request,
                                         MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED,
                                         "capacity_exceeded\n");
    }
    if (mem_service_read_payload(client_fd,
                                 request_payload,
                                 request.payload_len,
                                 request.payload_checksum) != 0) {
        return mem_service_send_response(client_fd,
                                         &request,
                                         MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH,
                                         "checksum_mismatch\n");
    }
    status = mem_service_handle_operation(
        svc,
        (enum mem_service_wire_operation)request.operation,
        (const char *)request_payload,
        response_payload,
        sizeof(response_payload),
        store_path,
        storage_root);
    return mem_service_send_response(client_fd,
                                     &request,
                                     status,
                                     response_payload[0] != '\0'
                                         ? response_payload
                                         : mem_service_wire_status_name(status));
}

static int mem_service_prepare_unix_addr(const char *path, struct sockaddr_un *addr)
{
    size_t path_len;

    if (path == NULL) {
        return -1;
    }
    path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, path_len + 1);
    return 0;
}

static int mem_service_parse_tcp_listen_spec(const char *listen_spec,
                                             struct sockaddr_in *addr)
{
    char endpoint[128];
    char *host;
    char *port_text;
    char *end = NULL;
    unsigned long port;
    size_t len;

    if (listen_spec == NULL || listen_spec[0] == '\0' || addr == NULL) {
        return -1;
    }
    if (strncmp(listen_spec,
                MEM_SERVICE_TCP_SPEC_PREFIX,
                strlen(MEM_SERVICE_TCP_SPEC_PREFIX)) == 0) {
        listen_spec += strlen(MEM_SERVICE_TCP_SPEC_PREFIX);
    }
    len = strlen(listen_spec);
    if (len == 0 || len >= sizeof(endpoint)) {
        return -1;
    }
    memcpy(endpoint, listen_spec, len + 1U);
    host = endpoint;
    port_text = strrchr(endpoint, ':');
    if (port_text == NULL || port_text == host) {
        return -1;
    }
    *port_text = '\0';
    port_text += 1;
    if (port_text[0] == '\0') {
        return -1;
    }
    errno = 0;
    port = strtoul(port_text, &end, 10);
    if (errno != 0 || end == port_text || *end != '\0' || port == 0UL ||
        port > 65535UL) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
        return -1;
    }
    return 0;
}

static int mem_service_open_metrics_listener(const char *listen_spec)
{
    struct sockaddr_in addr;
    int server_fd;
    int reuse = 1;

    if (mem_service_parse_tcp_listen_spec(listen_spec, &addr) != 0) {
        fprintf(stderr, "mem_service serve: invalid metrics listen path\n");
        return -1;
    }
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("mem_service serve: metrics socket");
        return -1;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(server_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("mem_service serve: metrics bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, 16) != 0) {
        perror("mem_service serve: metrics listen");
        close(server_fd);
        return -1;
    }
    return server_fd;
}

static int mem_service_http_write_all(int fd, const char *data, size_t data_len)
{
    size_t written = 0;

    while (written < data_len) {
        ssize_t rc = send(fd, data + written, data_len - written, 0);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        written += (size_t)rc;
    }
    return 0;
}

static int mem_service_http_send_response(int fd,
                                          unsigned int status,
                                          const char *reason,
                                          const char *content_type,
                                          const char *body)
{
    char header[512];
    size_t body_len = body != NULL ? strlen(body) : 0U;
    int header_len;

    if (reason == NULL || content_type == NULL) {
        return -1;
    }
    header_len = snprintf(header,
                          sizeof(header),
                          "HTTP/1.1 %u %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status,
                          reason,
                          content_type,
                          body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return -1;
    }
    if (mem_service_http_write_all(fd, header, (size_t)header_len) != 0) {
        return -1;
    }
    if (body_len > 0U && mem_service_http_write_all(fd, body, body_len) != 0) {
        return -1;
    }
    return 0;
}

static bool mem_service_metrics_export_key_is_safe(const char *key, size_t key_len)
{
    size_t i;

    if (key == NULL || key_len == 0U) {
        return false;
    }
    for (i = 0; i < key_len; ++i) {
        char c = key[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static const char *mem_service_metrics_export_type(const char *key, size_t key_len)
{
    const char *suffix = "_max_ms";
    size_t suffix_len = strlen(suffix);

    return key_len >= suffix_len &&
                   memcmp(key + key_len - suffix_len, suffix, suffix_len) == 0
               ? "gauge"
               : "counter";
}

static int mem_service_http_append_line(char *output,
                                        size_t output_len,
                                        size_t *used,
                                        const char *fmt,
                                        ...)
{
    va_list args;
    int rc;

    if (output == NULL || output_len == 0U || used == NULL || *used >= output_len) {
        return -1;
    }
    va_start(args, fmt);
    rc = vsnprintf(output + *used, output_len - *used, fmt, args);
    va_end(args);
    if (rc < 0 || (size_t)rc >= output_len - *used) {
        return -1;
    }
    *used += (size_t)rc;
    return 0;
}

static int mem_service_render_prometheus_text(const char *metrics_payload,
                                              char *output,
                                              size_t output_len)
{
    const char *cursor = metrics_payload;
    size_t used = 0;

    if (metrics_payload == NULL || output == NULL || output_len == 0U) {
        return -1;
    }
    output[0] = '\0';
    while (*cursor != '\0') {
        const char *equals = strchr(cursor, '=');
        const char *line_end = strchr(cursor, '\n');
        const char *value;
        size_t key_len;
        size_t value_len;

        if (line_end == NULL) {
            return -1;
        }
        if (equals == NULL || equals > line_end || equals == cursor) {
            return -1;
        }
        key_len = (size_t)(equals - cursor);
        value = equals + 1;
        value_len = (size_t)(line_end - value);
        if (!mem_service_metrics_export_key_is_safe(cursor, key_len)) {
            return -1;
        }
        if (mem_service_http_append_line(output,
                                         output_len,
                                         &used,
                                         "# HELP lingqu_mem_service_%.*s mem_service metric\n",
                                         (int)key_len,
                                         cursor) != 0 ||
            mem_service_http_append_line(output,
                                         output_len,
                                         &used,
                                         "# TYPE lingqu_mem_service_%.*s %s\n",
                                         (int)key_len,
                                         cursor,
                                         mem_service_metrics_export_type(cursor,
                                                                        key_len)) != 0 ||
            mem_service_http_append_line(output,
                                         output_len,
                                         &used,
                                         "lingqu_mem_service_%.*s %.*s\n",
                                         (int)key_len,
                                         cursor,
                                         (int)value_len,
                                         value) != 0) {
            return -1;
        }
        cursor = line_end + 1;
    }
    return 0;
}

static bool mem_service_http_request_is_get_metrics(const char *request)
{
    if (request == NULL) {
        return false;
    }
    return strncmp(request, "GET /metrics ", strlen("GET /metrics ")) == 0 &&
           strstr(request, "\r\n") != NULL;
}

static bool mem_service_http_request_is_method_get(const char *request)
{
    return request != NULL && strncmp(request, "GET ", strlen("GET ")) == 0;
}

static int mem_service_handle_metrics_http_client(int client_fd, struct mem_service *svc)
{
    char request[1024];
    char metrics_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char body[16384];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_OK;
    uint64_t start_ms = mem_service_monotonic_ms();
    ssize_t got;

    got = recv(client_fd, request, sizeof(request) - 1U, 0);
    if (got < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (got == 0) {
        return 0;
    }
    request[got] = '\0';
    if (!mem_service_http_request_is_get_metrics(request)) {
        status = MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
        if (mem_service_http_request_is_method_get(request)) {
            (void)mem_service_http_send_response(client_fd,
                                                 404U,
                                                 "Not Found",
                                                 "text/plain",
                                                 "not_found\n");
        } else {
            (void)mem_service_http_send_response(client_fd,
                                                 405U,
                                                 "Method Not Allowed",
                                                 "text/plain",
                                                 "method_not_allowed\n");
        }
        mem_service_record_operation_metrics(svc,
                                             MEM_SERVICE_WIRE_OP_METRICS,
                                             status,
                                             mem_service_monotonic_ms() - start_ms);
        return 0;
    }
    memset(metrics_payload, 0, sizeof(metrics_payload));
    memset(body, 0, sizeof(body));
    status = mem_service_metrics(svc, metrics_payload, sizeof(metrics_payload));
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        mem_service_render_prometheus_text(metrics_payload, body, sizeof(body)) != 0) {
        status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
        (void)mem_service_http_send_response(client_fd,
                                             500U,
                                             "Internal Server Error",
                                             "text/plain",
                                             "internal\n");
    } else {
        (void)mem_service_http_send_response(client_fd,
                                             200U,
                                             "OK",
                                             "text/plain; version=0.0.4",
                                             body);
    }
    mem_service_record_operation_metrics(svc,
                                         MEM_SERVICE_WIRE_OP_METRICS,
                                         status,
                                         mem_service_monotonic_ms() - start_ms);
    return status == MEM_SERVICE_WIRE_STATUS_OK ? 0 : -1;
}

int mem_service_run_unix_daemon_with_store_metrics_and_catalog(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root)
{
    struct mem_service svc;
    struct sockaddr_un addr;
    const char *path = mem_service_unix_path_from_spec(listen_spec);
    int server_fd;
    int metrics_fd = -1;
    int rc = 1;

    if (path == NULL && listen_spec == NULL) {
        path = MEM_SERVICE_DEFAULT_UNIX_SOCKET;
    }
    if (mem_service_prepare_unix_addr(path, &addr) != 0) {
        fprintf(stderr, "mem_service serve: invalid unix listen path\n");
        return 2;
    }
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service serve: init failed\n");
        return 1;
    }
    if (mem_service_prepare_durable_catalog_layout(storage_root) != 0) {
        fprintf(stderr,
                "mem_service serve: durable catalog layout failed root=%s\n",
                storage_root != NULL ? storage_root : "");
        return 1;
    }
    if (mem_service_load_durable_store(&svc, store_path) != 0) {
        fprintf(stderr, "mem_service serve: store load failed path=%s\n", store_path);
        return 1;
    }
    if (mem_service_write_durable_catalog_manifest(storage_root, store_path) != 0) {
        fprintf(stderr,
                "mem_service serve: durable catalog manifest failed root=%s\n",
                storage_root != NULL ? storage_root : "");
        return 1;
    }
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("mem_service serve: socket");
        return 1;
    }
    unlink(path);
    if (bind(server_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("mem_service serve: bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 16) != 0) {
        perror("mem_service serve: listen");
        close(server_fd);
        unlink(path);
        return 1;
    }
    if (metrics_listen_spec != NULL && metrics_listen_spec[0] != '\0') {
        metrics_fd = mem_service_open_metrics_listener(metrics_listen_spec);
        if (metrics_fd < 0) {
            close(server_fd);
            unlink(path);
            return 1;
        }
    }
    mem_service_daemon_stop = 0;
    if (mem_service_install_signal_handlers() != 0) {
        perror("mem_service serve: signal");
        close(server_fd);
        if (metrics_fd >= 0) {
            close(metrics_fd);
        }
        unlink(path);
        return 1;
    }
    if (store_path != NULL && store_path[0] != '\0') {
        printf("mem_service serve: status=ready listen=unix:%s store=%s records=%zu",
               path,
               store_path,
               svc.record_count);
    } else {
        printf("mem_service serve: status=ready listen=unix:%s records=%zu",
               path,
               svc.record_count);
    }
    if (metrics_fd >= 0) {
        printf(" metrics_listen=%s", metrics_listen_spec);
    }
    if (storage_root != NULL && storage_root[0] != '\0') {
        printf(" storage_root=%s", storage_root);
    }
    printf("\n");
    fflush(stdout);
    while (!mem_service_daemon_stop) {
        fd_set readfds;
        int max_fd = server_fd;
        int select_rc;

        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        if (metrics_fd >= 0) {
            FD_SET(metrics_fd, &readfds);
            if (metrics_fd > max_fd) {
                max_fd = metrics_fd;
            }
        }
        select_rc = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (select_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("mem_service serve: select");
            break;
        }
        if (FD_ISSET(server_fd, &readfds)) {
            int client_fd = accept(server_fd, NULL, NULL);

            if (client_fd < 0) {
                if (errno != EINTR) {
                    perror("mem_service serve: accept");
                    break;
                }
            } else {
                if (mem_service_handle_client(client_fd,
                                              &svc,
                                              store_path,
                                              storage_root) != 0) {
                    fprintf(stderr, "mem_service serve: client request failed\n");
                }
                close(client_fd);
            }
        }
        if (metrics_fd >= 0 && FD_ISSET(metrics_fd, &readfds)) {
            int client_fd = accept(metrics_fd, NULL, NULL);

            if (client_fd < 0) {
                if (errno != EINTR) {
                    perror("mem_service serve: metrics accept");
                    break;
                }
            } else {
                if (mem_service_handle_metrics_http_client(client_fd, &svc) != 0) {
                    fprintf(stderr, "mem_service serve: metrics request failed\n");
                }
                close(client_fd);
            }
        }
    }
    rc = 0;
    close(server_fd);
    if (metrics_fd >= 0) {
        close(metrics_fd);
    }
    unlink(path);
    printf("mem_service serve: status=stopped\n");
    return rc;
}

int mem_service_run_unix_daemon_with_store_and_metrics(const char *listen_spec,
                                                       const char *store_path,
                                                       const char *metrics_listen_spec)
{
    return mem_service_run_unix_daemon_with_store_metrics_and_catalog(
        listen_spec,
        store_path,
        metrics_listen_spec,
        NULL);
}

int mem_service_run_unix_daemon_with_store(const char *listen_spec, const char *store_path)
{
    return mem_service_run_unix_daemon_with_store_and_metrics(listen_spec,
                                                             store_path,
                                                             NULL);
}

int mem_service_run_unix_daemon(const char *listen_spec)
{
    return mem_service_run_unix_daemon_with_store(listen_spec, NULL);
}
