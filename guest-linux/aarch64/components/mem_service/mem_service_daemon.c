#include "mem_service_daemon.h"

#include <errno.h>
#include <fcntl.h>
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
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
#define MEM_SERVICE_STORE_SCHEMA_VERSION 1
#define MEM_SERVICE_STORE_MAX_KNOWN_SCHEMA_VERSION 1
#define MEM_SERVICE_DURABLE_CATALOG_MAGIC "mem_service_durable_catalog_v1"
#define MEM_SERVICE_DURABLE_CATALOG_MANIFEST "manifest.txt"
#define MEM_SERVICE_DURABLE_CATALOG_SCHEMA_VERSION 1
#define MEM_SERVICE_DURABLE_CATALOG_MAX_KNOWN_VERSION 1
#define MEM_SERVICE_JOURNAL_COMPACTION_THRESHOLD_BYTES 4096U
#define MEM_SERVICE_CHUNKED_BLOCK_SIZE 1024U
#define MEM_SERVICE_CHUNKED_BLOCK_DIR_SUFFIX ".chunked"
#define MEM_SERVICE_CHUNKED_BLOCK_MANIFEST "manifest.txt"
#define MEM_SERVICE_TRANSPORT_BLOCK_DIR_SUFFIX ".transport"
#define MEM_SERVICE_TRANSPORT_TCP_BLOCK_DIR_SUFFIX ".tcp"
#define MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD "payload.block"
#define MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST "manifest.txt"
#define MEM_SERVICE_SNAPSHOT_PAGE_HEADER_RESERVE 512U
#define MEM_SERVICE_UB_SSD_DEFAULT_DEVICE "/dev/ub_ssd0"
#define MEM_SERVICE_UB_SSD_IOC_MAGIC 'S'
#define MEM_SERVICE_UB_SSD_OP_BLOCK_WRITE 1U
#define MEM_SERVICE_UB_SSD_OP_BLOCK_READ 2U
#define MEM_SERVICE_UB_SSD_OK 0
#define MEM_SERVICE_UB_SSD_ERR_STALE_EPOCH (-6)
#define MEM_SERVICE_UB_SSD_ERR_SEGMENT_RETIRED (-7)
#define MEM_SERVICE_UB_SSD_ERR_COH_TIMEOUT (-8)
#define MEM_SERVICE_UB_SSD_ERR_CHECKSUM (-10)
#define MEM_SERVICE_UB_SSD_ERR_VERSION_CONFLICT (-11)

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
static enum mem_service_wire_status mem_service_handle_operation_with_limits(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    const char *store_path,
    const char *storage_root,
    const struct mem_service_daemon_limits *limits);
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
static uint64_t mem_service_estimate_new_record_count(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload);
static bool mem_service_request_exceeds_payload_limit(
    uint32_t payload_len,
    const struct mem_service_daemon_limits *limits);
static bool mem_service_payload_get_u64_checked(const char *payload,
                                                const char *name,
                                                uint64_t *out);
static int mem_service_save_store(const struct mem_service *svc,
                                  const char *store_path);
static int mem_service_compact_journal(const char *store_path);
static int mem_service_compact_journal_now(const char *store_path);
static int mem_service_parse_schema_version_line(const char *line,
                                                 const char *prefix,
                                                 long max_known_version,
                                                 bool *saw_version_out);
static int mem_service_write_durable_catalog_manifest(const char *storage_root,
                                                      const char *store_path);
static uint64_t mem_service_audit_first_sequence(const struct mem_service *svc);
static bool mem_service_apply_audit_retention(struct mem_service *svc,
                                              uint64_t max_audit_events);
static bool mem_service_apply_checkpoint_retention(struct mem_service *svc,
                                                   uint64_t max_checkpoint_records,
                                                   const char *storage_root,
                                                   uint64_t *payload_gc_out);
static bool mem_service_apply_record_retention(struct mem_service *svc,
                                               uint64_t max_retained_records,
                                               uint64_t max_retained_record_age_ms,
                                               uint32_t retained_record_kind,
                                               bool retained_record_tenant_enabled,
                                               uint32_t retained_record_tenant,
                                               const char *storage_root,
                                               uint64_t *payload_gc_out);
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

struct mem_service_ub_ssd_key_v1 {
    uint32_t version;
    uint32_t flags;
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t size;
    uint64_t vmid;
    uint64_t asid;
    uint64_t pte_offset;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint64_t epoch;
} __attribute__((packed));

struct mem_service_ub_ssd_block_ref_v1 {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
} __attribute__((packed));

struct mem_service_ub_ssd_buffer_desc_v1 {
    uint64_t gsva_base;
    uint64_t bytes;
    struct mem_service_ub_ssd_key_v1 key;
    uint32_t token_id;
    uint32_t token_value;
} __attribute__((packed));

struct mem_service_ub_ssd_cmd_v1 {
    uint32_t version;
    uint32_t opcode;
    uint64_t req_id;
    uint32_t source_cna;
    uint32_t target_ssd_cna;
    uint32_t flags;
    struct mem_service_ub_ssd_block_ref_v1 block_ref;
    struct mem_service_ub_ssd_buffer_desc_v1 buffer;
} __attribute__((packed));

struct mem_service_ub_ssd_cpl_v1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    struct mem_service_ub_ssd_block_ref_v1 committed_ref;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
} __attribute__((packed));

#ifdef __linux__
#define MEM_SERVICE_UB_SSD_SUBMIT \
    _IOW(MEM_SERVICE_UB_SSD_IOC_MAGIC, 1, struct mem_service_ub_ssd_cmd_v1)
#define MEM_SERVICE_UB_SSD_WAIT \
    _IOR(MEM_SERVICE_UB_SSD_IOC_MAGIC, 2, struct mem_service_ub_ssd_cpl_v1)
#endif

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
         101,
         0xd8ec3bdaU},
        {"export_snapshot_page_response",
         MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
         fixtures[18].payload,
         MEM_SERVICE_WIRE_STATUS_OK,
         146,
         0xb6c05123U},
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
    } else if (strcmp(name, "object_backend_kind") == 0) {
        record->object_backend_kind = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "object_backend_node") == 0) {
        record->object_backend_node = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "object_backend_device_cna") == 0) {
        record->object_backend_device_cna = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "object_backend_flags") == 0) {
        record->object_backend_flags = mem_service_parse_u32_value(value, 0);
    } else if (strcmp(name, "object_backend_block_hi") == 0) {
        record->object_backend_block_hi = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_backend_block_lo") == 0) {
        record->object_backend_block_lo = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_backend_block_version") == 0) {
        record->object_backend_block_version = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_backend_block_offset") == 0) {
        record->object_backend_block_offset = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_backend_block_bytes") == 0) {
        record->object_backend_block_bytes = mem_service_parse_u64_value(value, 0);
    } else if (strcmp(name, "object_backend_block_checksum") == 0) {
        record->object_backend_block_checksum = mem_service_parse_u64_value(value, 0);
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
    bool saw_schema_version = false;

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
        } else {
            if (!state.in_record && !state.in_idempotency && !state.in_audit) {
                int schema_rc = mem_service_parse_schema_version_line(
                    line,
                    "store_schema_version=",
                    MEM_SERVICE_STORE_MAX_KNOWN_SCHEMA_VERSION,
                    &saw_schema_version);

                if (schema_rc < 0) {
                    return -1;
                }
                if (schema_rc > 0) {
                    if (newline == NULL) {
                        break;
                    }
                    cursor = newline + 1;
                    continue;
                }
            }
            if (mem_service_import_store_line(svc, line, &state) != 0) {
                return -1;
            }
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
    bool saw_schema_version = false;

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
        if (!state.in_record && !state.in_idempotency && !state.in_audit) {
            int schema_rc = mem_service_parse_schema_version_line(
                line,
                "store_schema_version=",
                MEM_SERVICE_STORE_MAX_KNOWN_SCHEMA_VERSION,
                &saw_schema_version);

            if (schema_rc < 0) {
                return -1;
            }
            if (schema_rc > 0) {
                if (newline == NULL) {
                    break;
                }
                cursor = newline + 1;
                continue;
            }
        }
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

static int mem_service_parse_schema_version_line(const char *line,
                                                 const char *prefix,
                                                 long max_known_version,
                                                 bool *saw_version_out)
{
    long parsed;
    char *end = NULL;
    size_t prefix_len;

    if (line == NULL || prefix == NULL || saw_version_out == NULL) {
        return -1;
    }
    prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0) {
        return 0;
    }
    parsed = strtol(line + prefix_len, &end, 10);
    *saw_version_out = true;
    if (end == line + prefix_len || *end != '\0') {
        return -1;
    }
    return parsed <= 0 || parsed > max_known_version ? -1 : 1;
}

static int mem_service_admit_or_migrate_catalog_schema_version(
    const char *storage_root,
    const char *store_path)
{
    char manifest_path[512];
    char line[512];
    FILE *file;
    bool saw_schema_version = false;

    if (storage_root == NULL || storage_root[0] == '\0') {
        return 0;
    }
    if (mem_service_make_catalog_path(storage_root,
                                      MEM_SERVICE_DURABLE_CATALOG_MANIFEST,
                                      manifest_path,
                                      sizeof(manifest_path)) != 0) {
        return 0;
    }
    file = fopen(manifest_path, "r");
    if (file == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        mem_service_trim_line(line);
        {
            int schema_rc = mem_service_parse_schema_version_line(
                line,
                "catalog_schema_version=",
                MEM_SERVICE_DURABLE_CATALOG_MAX_KNOWN_VERSION,
                &saw_schema_version);
            if (schema_rc != 0) {
                fclose(file);
                return schema_rc < 0 ? -1 : 0;
            }
        }
    }
    fclose(file);
    if (!saw_schema_version && store_path != NULL && store_path[0] != '\0') {
        return mem_service_write_durable_catalog_manifest(storage_root, store_path);
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
                       "catalog_schema_version=%d\n"
                       "catalog_dir=%s\n"
                       "block_dir=%s\n"
                       "quarantine_dir=%s\n"
                       "store_path=%s\n"
                       "journal_path=%s\n"
                       "store_magic=%s\n"
                       "journal_magic=%s\n"
                       "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1\n"
                       "corrupt_payload_policy=quarantine-fail-closed\n",
                       MEM_SERVICE_DURABLE_CATALOG_MAGIC,
                       MEM_SERVICE_DURABLE_CATALOG_SCHEMA_VERSION,
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
                                             size_t path_len);
static enum mem_service_wire_status mem_service_copy_payload_file_to_tmp(
    const char *payload_path,
    const char *tmp_path,
    uint64_t *actual_len_out,
    uint64_t *actual_checksum_out);

static int mem_service_make_chunked_block_dir_path(const char *storage_root,
                                                   uint64_t checksum,
                                                   char *path,
                                                   size_t path_len)
{
    char block_dir[512];
    char dir_name[64];

    if (mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "blocks") != 0 ||
        snprintf(dir_name,
                 sizeof(dir_name),
                 "%016" PRIx64 "%s",
                 checksum,
                 MEM_SERVICE_CHUNKED_BLOCK_DIR_SUFFIX) >= (int)sizeof(dir_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, block_dir, dir_name);
}

static int mem_service_make_chunked_block_chunk_path(const char *dir_path,
                                                     uint32_t index,
                                                     char *path,
                                                     size_t path_len)
{
    char chunk_name[32];

    if (snprintf(chunk_name,
                 sizeof(chunk_name),
                 "%04u.chunk",
                 index) >= (int)sizeof(chunk_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, dir_path, chunk_name);
}

static void mem_service_quarantine_chunked_payload_block(const char *storage_root,
                                                         uint64_t checksum)
{
    char dir_path[512];
    char quarantine_dir[512];
    char quarantine_name[80];
    char quarantine_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        mem_service_make_chunked_block_dir_path(storage_root,
                                                checksum,
                                                dir_path,
                                                sizeof(dir_path)) != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        snprintf(quarantine_name,
                 sizeof(quarantine_name),
                 "%016" PRIx64 "%s.bad.%ld",
                 checksum,
                 MEM_SERVICE_CHUNKED_BLOCK_DIR_SUFFIX,
                 (long)getpid()) >= (int)sizeof(quarantine_name) ||
        mem_service_join_path(quarantine_path,
                              sizeof(quarantine_path),
                              quarantine_dir,
                              quarantine_name) != 0) {
        return;
    }
    (void)mem_service_ensure_dir(quarantine_dir);
    (void)rename(dir_path, quarantine_path);
}

static int mem_service_make_transport_block_dir_path(const char *storage_root,
                                                     uint64_t checksum,
                                                     char *path,
                                                     size_t path_len)
{
    char block_dir[512];
    char dir_name[64];

    if (mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "remote-blocks") != 0 ||
        snprintf(dir_name,
                 sizeof(dir_name),
                 "%016" PRIx64 "%s",
                 checksum,
                 MEM_SERVICE_TRANSPORT_BLOCK_DIR_SUFFIX) >= (int)sizeof(dir_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, block_dir, dir_name);
}

static int mem_service_make_transport_tcp_block_dir_path(const char *storage_root,
                                                         uint64_t checksum,
                                                         char *path,
                                                         size_t path_len)
{
    char block_dir[512];
    char dir_name[64];

    if (mem_service_join_path(block_dir,
                              sizeof(block_dir),
                              storage_root,
                              "remote-blocks") != 0 ||
        snprintf(dir_name,
                 sizeof(dir_name),
                 "%016" PRIx64 "%s",
                 checksum,
                 MEM_SERVICE_TRANSPORT_TCP_BLOCK_DIR_SUFFIX) >=
            (int)sizeof(dir_name)) {
        return -1;
    }
    return mem_service_join_path(path, path_len, block_dir, dir_name);
}

static void mem_service_quarantine_transport_payload_block(const char *storage_root,
                                                           uint64_t checksum)
{
    char dir_path[512];
    char quarantine_dir[512];
    char quarantine_name[80];
    char quarantine_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        mem_service_make_transport_block_dir_path(storage_root,
                                                  checksum,
                                                  dir_path,
                                                  sizeof(dir_path)) != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        snprintf(quarantine_name,
                 sizeof(quarantine_name),
                 "%016" PRIx64 "%s.bad.%ld",
                 checksum,
                 MEM_SERVICE_TRANSPORT_BLOCK_DIR_SUFFIX,
                 (long)getpid()) >= (int)sizeof(quarantine_name) ||
        mem_service_join_path(quarantine_path,
                              sizeof(quarantine_path),
                              quarantine_dir,
                              quarantine_name) != 0) {
        return;
    }
    (void)mem_service_ensure_dir(quarantine_dir);
    (void)rename(dir_path, quarantine_path);
}

static void mem_service_quarantine_transport_tcp_payload_block(
    const char *storage_root,
    uint64_t checksum)
{
    char dir_path[512];
    char quarantine_dir[512];
    char quarantine_name[80];
    char quarantine_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        mem_service_make_transport_tcp_block_dir_path(storage_root,
                                                      checksum,
                                                      dir_path,
                                                      sizeof(dir_path)) != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        snprintf(quarantine_name,
                 sizeof(quarantine_name),
                 "%016" PRIx64 "%s.bad.%ld",
                 checksum,
                 MEM_SERVICE_TRANSPORT_TCP_BLOCK_DIR_SUFFIX,
                 (long)getpid()) >= (int)sizeof(quarantine_name) ||
        mem_service_join_path(quarantine_path,
                              sizeof(quarantine_path),
                              quarantine_dir,
                              quarantine_name) != 0) {
        return;
    }
    (void)mem_service_ensure_dir(quarantine_dir);
    (void)rename(dir_path, quarantine_path);
}

static bool mem_service_record_has_payload_block(
    const struct mem_service_record *record)
{
    if (record == NULL || !record->in_use ||
        record->object_payload_checksum == 0U) {
        return false;
    }
    return record->object_payload_kind == MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK ||
           record->object_payload_kind == MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK ||
           record->object_payload_kind == MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK ||
           record->object_payload_kind == MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK;
}

static bool mem_service_payload_block_is_referenced(
    const struct mem_service *svc,
    const struct mem_service_record *record,
    size_t skip_index)
{
    size_t i;

    if (svc == NULL || !mem_service_record_has_payload_block(record)) {
        return false;
    }
    for (i = 0U; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *candidate = &svc->records[i];

        if (i == skip_index || !candidate->in_use) {
            continue;
        }
        if (candidate->object_payload_kind == record->object_payload_kind &&
            candidate->object_payload_checksum == record->object_payload_checksum) {
            return true;
        }
    }
    return false;
}

static int mem_service_unlink_if_exists(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    if (unlink(path) == 0) {
        return 1;
    }
    return errno == ENOENT ? 0 : -1;
}

static int mem_service_remove_sealed_local_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char block_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        record == NULL ||
        mem_service_make_payload_block_path(storage_root,
                                            record->object_payload_checksum,
                                            block_path,
                                            sizeof(block_path)) != 0) {
        return -1;
    }
    return mem_service_unlink_if_exists(block_path);
}

static int mem_service_remove_chunked_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char dir_path[512];
    char manifest_path[512];
    uint64_t chunks;
    uint64_t index;
    int removed = 0;

    if (storage_root == NULL || storage_root[0] == '\0' ||
        record == NULL ||
        mem_service_make_chunked_block_dir_path(storage_root,
                                                record->object_payload_checksum,
                                                dir_path,
                                                sizeof(dir_path)) != 0) {
        return -1;
    }
    chunks = (record->object_backing_len + MEM_SERVICE_CHUNKED_BLOCK_SIZE - 1U) /
             MEM_SERVICE_CHUNKED_BLOCK_SIZE;
    for (index = 0U; index < chunks; ++index) {
        char chunk_path[512];
        int rc;

        if (index > UINT32_MAX ||
            mem_service_make_chunked_block_chunk_path(dir_path,
                                                      (uint32_t)index,
                                                      chunk_path,
                                                      sizeof(chunk_path)) != 0) {
            return -1;
        }
        rc = mem_service_unlink_if_exists(chunk_path);
        if (rc < 0) {
            return -1;
        }
        removed += rc;
    }
    if (mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_CHUNKED_BLOCK_MANIFEST) != 0) {
        return -1;
    }
    {
        int rc = mem_service_unlink_if_exists(manifest_path);

        if (rc < 0) {
            return -1;
        }
        removed += rc;
    }
    if (rmdir(dir_path) == 0) {
        removed += 1;
    } else if (errno != ENOENT) {
        return -1;
    }
    return removed > 0 ? 1 : 0;
}

static int mem_service_remove_transport_payload_block_dir(
    const char *dir_path)
{
    char manifest_path[512];
    char payload_path[512];
    int removed = 0;
    int rc;

    if (dir_path == NULL || dir_path[0] == '\0' ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        mem_service_join_path(payload_path,
                              sizeof(payload_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0) {
        return -1;
    }
    rc = mem_service_unlink_if_exists(payload_path);
    if (rc < 0) {
        return -1;
    }
    removed += rc;
    rc = mem_service_unlink_if_exists(manifest_path);
    if (rc < 0) {
        return -1;
    }
    removed += rc;
    if (rmdir(dir_path) == 0) {
        removed += 1;
    } else if (errno != ENOENT) {
        return -1;
    }
    return removed > 0 ? 1 : 0;
}

static int mem_service_remove_transport_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char dir_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        record == NULL ||
        mem_service_make_transport_block_dir_path(storage_root,
                                                  record->object_payload_checksum,
                                                  dir_path,
                                                  sizeof(dir_path)) != 0) {
        return -1;
    }
    return mem_service_remove_transport_payload_block_dir(dir_path);
}

static int mem_service_remove_transport_tcp_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char dir_path[512];

    if (storage_root == NULL || storage_root[0] == '\0' ||
        record == NULL ||
        mem_service_make_transport_tcp_block_dir_path(storage_root,
                                                      record->object_payload_checksum,
                                                      dir_path,
                                                      sizeof(dir_path)) != 0) {
        return -1;
    }
    return mem_service_remove_transport_payload_block_dir(dir_path);
}

static bool mem_service_gc_payload_block_if_orphaned(
    const struct mem_service *svc,
    size_t record_index,
    const char *storage_root,
    uint64_t *payload_gc_out)
{
    const struct mem_service_record *record;
    int rc = 0;

    if (svc == NULL || record_index >= MEM_SERVICE_MAX_RECORDS ||
        storage_root == NULL || storage_root[0] == '\0') {
        return false;
    }
    record = &svc->records[record_index];
    if (!mem_service_record_has_payload_block(record) ||
        mem_service_payload_block_is_referenced(svc, record, record_index)) {
        return false;
    }
    if (record->object_payload_kind == MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK) {
        rc = mem_service_remove_sealed_local_payload_block(storage_root, record);
    } else if (record->object_payload_kind ==
               MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK) {
        rc = mem_service_remove_chunked_payload_block(storage_root, record);
    } else if (record->object_payload_kind ==
               MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK) {
        rc = mem_service_remove_transport_payload_block(storage_root, record);
    } else if (record->object_payload_kind ==
               MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK) {
        rc = mem_service_remove_transport_tcp_payload_block(storage_root, record);
    }
    if (rc > 0 && payload_gc_out != NULL) {
        *payload_gc_out += 1U;
    }
    return rc > 0;
}

static int mem_service_parse_tcp_payload_source(const char *source,
                                                struct sockaddr_in *addr)
{
    char endpoint[160];
    char *host;
    char *port_text;
    char *end = NULL;
    unsigned long port;
    size_t len;

    if (source == NULL || addr == NULL) {
        return -1;
    }
    if (strncmp(source,
                MEM_SERVICE_TCP_SPEC_PREFIX,
                strlen(MEM_SERVICE_TCP_SPEC_PREFIX)) == 0) {
        source += strlen(MEM_SERVICE_TCP_SPEC_PREFIX);
    }
    len = strlen(source);
    if (len == 0U || len >= sizeof(endpoint)) {
        return -1;
    }
    memcpy(endpoint, source, len + 1U);
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
    port = strtoul(port_text, &end, 10);
    if (end == NULL || *end != '\0' || port == 0UL || port > 65535UL) {
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

static enum mem_service_wire_status mem_service_fetch_tcp_payload_to_tmp(
    const char *source,
    const char *tmp_path,
    uint64_t *actual_len_out,
    uint64_t *actual_checksum_out)
{
    struct sockaddr_in addr;
    uint8_t buffer[4096];
    uint64_t actual_len = 0;
    uint64_t hash = 1469598103934665603ULL;
    int fd;
    FILE *dst;

    if (source == NULL || source[0] == '\0' ||
        tmp_path == NULL || tmp_path[0] == '\0' ||
        actual_len_out == NULL || actual_checksum_out == NULL) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_parse_tcp_payload_source(source, &addr) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    dst = fopen(tmp_path, "wb");
    if (dst == NULL) {
        close(fd);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        ssize_t i;

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            fclose(dst);
            close(fd);
            unlink(tmp_path);
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        if (got == 0) {
            break;
        }
        if (fwrite(buffer, 1U, (size_t)got, dst) != (size_t)got) {
            fclose(dst);
            close(fd);
            unlink(tmp_path);
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        for (i = 0; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
        actual_len += (uint64_t)got;
    }
    close(fd);
    if (fclose(dst) != 0) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    *actual_len_out = actual_len;
    *actual_checksum_out = hash;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_write_transport_payload_block(
    const char *storage_root,
    const char *payload,
    const char *payload_inline,
    const char *payload_path,
    struct mem_service_record *record)
{
    char tmp_path[512];
    char dir_path[512];
    char block_path[512];
    char manifest_path[512];
    char manifest_tmp[512];
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
    if (mem_service_make_payload_tmp_path(storage_root,
                                          tmp_path,
                                          sizeof(tmp_path)) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (has_inline) {
        actual_len = (uint64_t)strlen(payload_inline);
        actual_checksum =
            mem_service_checksum_bytes((const uint8_t *)payload_inline, actual_len);
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
    } else {
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
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_payload_get_u64_checked(payload, "checksum", &expected_checksum) &&
        expected_checksum != actual_checksum) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_make_transport_block_dir_path(storage_root,
                                                  actual_checksum,
                                                  dir_path,
                                                  sizeof(dir_path)) != 0 ||
        mem_service_ensure_dir(dir_path) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0 ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        snprintf(manifest_tmp,
                 sizeof(manifest_tmp),
                 "%s.tmp.%ld",
                 manifest_path,
                 (long)getpid()) >= (int)sizeof(manifest_tmp)) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (rename(tmp_path, block_path) != 0) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    file = fopen(manifest_tmp, "w");
    if (file == NULL ||
        fprintf(file,
                "backend=transport-loopback-block-v1\n"
                "transport=file-copy-v1\n"
                "payload=%s\n"
                "total_len=%" PRIu64 "\n"
                "total_checksum=0x%016" PRIx64 "\n",
                MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD,
                actual_len,
                actual_checksum) < 0 ||
        fclose(file) != 0 ||
        rename(manifest_tmp, manifest_path) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        unlink(manifest_tmp);
        mem_service_quarantine_transport_payload_block(storage_root,
                                                       actual_checksum);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK;
    record->object_backing_offset = 0;
    record->object_backing_len = actual_len;
    record->object_payload_checksum = actual_checksum;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_write_transport_tcp_payload_block(
    const char *storage_root,
    const char *payload,
    const char *payload_inline,
    const char *payload_path,
    struct mem_service_record *record)
{
    char tmp_path[512];
    char dir_path[512];
    char block_path[512];
    char manifest_path[512];
    char manifest_tmp[512];
    uint64_t expected_len = 0;
    uint64_t expected_checksum = 0;
    uint64_t actual_len = 0;
    uint64_t actual_checksum = 0;
    bool has_inline = payload_inline != NULL && payload_inline[0] != '\0';
    bool has_path = payload_path != NULL && payload_path[0] != '\0';
    FILE *file;
    enum mem_service_wire_status status;

    if (has_inline || !has_path) {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (strncmp(payload_path,
                MEM_SERVICE_TCP_SPEC_PREFIX,
                strlen(MEM_SERVICE_TCP_SPEC_PREFIX)) != 0) {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (record == NULL || storage_root == NULL || storage_root[0] == '\0') {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (mem_service_make_payload_tmp_path(storage_root,
                                          tmp_path,
                                          sizeof(tmp_path)) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    status = mem_service_fetch_tcp_payload_to_tmp(payload_path,
                                                  tmp_path,
                                                  &actual_len,
                                                  &actual_checksum);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        return status;
    }
    if (mem_service_payload_get_u64_checked(payload, "backing_len", &expected_len) &&
        expected_len != actual_len) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_payload_get_u64_checked(payload, "checksum", &expected_checksum) &&
        expected_checksum != actual_checksum) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_make_transport_tcp_block_dir_path(storage_root,
                                                      actual_checksum,
                                                      dir_path,
                                                      sizeof(dir_path)) != 0 ||
        mem_service_ensure_dir(dir_path) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0 ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        snprintf(manifest_tmp,
                 sizeof(manifest_tmp),
                 "%s.tmp.%ld",
                 manifest_path,
                 (long)getpid()) >= (int)sizeof(manifest_tmp)) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (rename(tmp_path, block_path) != 0) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    file = fopen(manifest_tmp, "w");
    if (file == NULL ||
        fprintf(file,
                "backend=transport-tcp-block-v1\n"
                "transport=tcp-loopback-v1\n"
                "payload=%s\n"
                "total_len=%" PRIu64 "\n"
                "total_checksum=0x%016" PRIx64 "\n",
                MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD,
                actual_len,
                actual_checksum) < 0 ||
        fclose(file) != 0 ||
        rename(manifest_tmp, manifest_path) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        unlink(manifest_tmp);
        mem_service_quarantine_transport_tcp_payload_block(storage_root,
                                                           actual_checksum);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK;
    record->object_backing_offset = 0;
    record->object_backing_len = actual_len;
    record->object_payload_checksum = actual_checksum;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_write_chunked_payload_block(
    const char *storage_root,
    const char *payload,
    const char *payload_inline,
    const char *payload_path,
    struct mem_service_record *record)
{
    char tmp_path[512];
    char dir_path[512];
    char manifest_path[512];
    char manifest_tmp[512];
    char chunk_path[512];
    char chunk_tmp[512];
    uint64_t expected_len = 0;
    uint64_t expected_checksum = 0;
    uint64_t actual_len = 0;
    uint64_t actual_checksum = 0;
    bool has_inline = payload_inline != NULL && payload_inline[0] != '\0';
    bool has_path = payload_path != NULL && payload_path[0] != '\0';
    FILE *file;
    FILE *src;
    uint8_t buffer[MEM_SERVICE_CHUNKED_BLOCK_SIZE];
    uint32_t chunk_index = 0U;
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
    if (mem_service_make_payload_tmp_path(storage_root,
                                          tmp_path,
                                          sizeof(tmp_path)) != 0) {
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    if (has_inline) {
        actual_len = (uint64_t)strlen(payload_inline);
        actual_checksum =
            mem_service_checksum_bytes((const uint8_t *)payload_inline, actual_len);
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
    } else {
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
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_payload_get_u64_checked(payload, "checksum", &expected_checksum) &&
        expected_checksum != actual_checksum) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_make_chunked_block_dir_path(storage_root,
                                                actual_checksum,
                                                dir_path,
                                                sizeof(dir_path)) != 0 ||
        mem_service_ensure_dir(dir_path) != 0) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    src = fopen(tmp_path, "rb");
    if (src == NULL) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    for (;;) {
        size_t got = fread(buffer, 1U, sizeof(buffer), src);
        char seq_tail[32];
        uint64_t seq = ++mem_service_payload_tmp_seq;

        if (got > 0U) {
            FILE *chunk_file;

            if (mem_service_make_chunked_block_chunk_path(dir_path,
                                                          chunk_index,
                                                          chunk_path,
                                                          sizeof(chunk_path)) != 0 ||
                snprintf(seq_tail,
                         sizeof(seq_tail),
                         ".%lu.%" PRIu64,
                         (long)getpid(),
                         seq) >= (int)sizeof(seq_tail) ||
                snprintf(chunk_tmp,
                         sizeof(chunk_tmp),
                         "%s.tmp%s",
                         chunk_path,
                         seq_tail) >= (int)sizeof(chunk_tmp)) {
                fclose(src);
                unlink(tmp_path);
                return MEM_SERVICE_WIRE_STATUS_INTERNAL;
            }
            chunk_file = fopen(chunk_tmp, "wb");
            if (chunk_file == NULL ||
                fwrite(buffer, 1U, got, chunk_file) != got ||
                fclose(chunk_file) != 0 ||
                rename(chunk_tmp, chunk_path) != 0) {
                if (chunk_file != NULL) {
                    fclose(chunk_file);
                }
                unlink(chunk_tmp);
                fclose(src);
                unlink(tmp_path);
                return MEM_SERVICE_WIRE_STATUS_INTERNAL;
            }
            chunk_index += 1U;
        }
        if (got < sizeof(buffer)) {
            if (ferror(src)) {
                fclose(src);
                unlink(tmp_path);
                return MEM_SERVICE_WIRE_STATUS_INTERNAL;
            }
            break;
        }
    }
    fclose(src);
    if (mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_CHUNKED_BLOCK_MANIFEST) != 0 ||
        snprintf(manifest_tmp,
                 sizeof(manifest_tmp),
                 "%s.tmp.%ld",
                 manifest_path,
                 (long)getpid()) >= (int)sizeof(manifest_tmp)) {
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    file = fopen(manifest_tmp, "w");
    if (file == NULL ||
        fprintf(file,
                "chunk_count=%u\n"
                "chunk_size=%u\n"
                "total_len=%" PRIu64 "\n"
                "total_checksum=0x%016" PRIx64 "\n",
                chunk_index,
                MEM_SERVICE_CHUNKED_BLOCK_SIZE,
                actual_len,
                actual_checksum) < 0 ||
        fclose(file) != 0 ||
        rename(manifest_tmp, manifest_path) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        unlink(manifest_tmp);
        unlink(tmp_path);
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    unlink(tmp_path);
    record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK;
    record->object_backing_offset = 0;
    record->object_backing_len = actual_len;
    record->object_payload_checksum = actual_checksum;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_validate_chunked_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char dir_path[512];
    char manifest_path[512];
    char chunk_path[512];
    char line[128];
    uint64_t hash = 1469598103934665603ULL;
    uint64_t actual_len = 0U;
    uint64_t manifest_total_checksum = 0U;
    uint64_t parsed_checksum = 0U;
    uint32_t chunk_count = 0U;
    uint32_t chunk_size = 0U;
    uint32_t index;
    FILE *manifest_file;
    FILE *chunk_file;
    bool saw_chunk_count = false;
    bool saw_total_checksum = false;

    if (record == NULL ||
        record->object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (storage_root == NULL || storage_root[0] == '\0' ||
        record->object_payload_checksum == 0U ||
        mem_service_make_chunked_block_dir_path(storage_root,
                                                record->object_payload_checksum,
                                                dir_path,
                                                sizeof(dir_path)) != 0) {
        mem_service_quarantine_chunked_payload_block(storage_root,
                                                     record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_CHUNKED_BLOCK_MANIFEST) != 0) {
        mem_service_quarantine_chunked_payload_block(storage_root,
                                                     record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    manifest_file = fopen(manifest_path, "r");
    if (manifest_file == NULL) {
        mem_service_quarantine_chunked_payload_block(storage_root,
                                                     record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    while (fgets(line, sizeof(line), manifest_file) != NULL) {
        mem_service_trim_line(line);
        if (strncmp(line, "chunk_count=", sizeof("chunk_count=") - 1U) == 0) {
            chunk_count = (uint32_t)strtoul(
                line + sizeof("chunk_count=") - 1U, NULL, 10);
            saw_chunk_count = true;
        } else if (strncmp(line, "chunk_size=", sizeof("chunk_size=") - 1U) == 0) {
            chunk_size = (uint32_t)strtoul(
                line + sizeof("chunk_size=") - 1U, NULL, 10);
        } else if (strncmp(line,
                           "total_checksum=0x",
                           sizeof("total_checksum=0x") - 1U) == 0) {
            parsed_checksum = (uint64_t)strtoull(
                line + sizeof("total_checksum=0x") - 1U, NULL, 16);
            manifest_total_checksum = parsed_checksum;
            saw_total_checksum = true;
        }
    }
    fclose(manifest_file);
    if (!saw_chunk_count || !saw_total_checksum || chunk_count == 0U ||
        chunk_size == 0U || chunk_size > MEM_SERVICE_CHUNKED_BLOCK_SIZE) {
        mem_service_quarantine_chunked_payload_block(storage_root,
                                                     record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    for (index = 0U; index < chunk_count; ++index) {
        uint8_t buffer[MEM_SERVICE_CHUNKED_BLOCK_SIZE];

        if (mem_service_make_chunked_block_chunk_path(dir_path,
                                                      index,
                                                      chunk_path,
                                                      sizeof(chunk_path)) != 0) {
            mem_service_quarantine_chunked_payload_block(
                storage_root, record->object_payload_checksum);
            return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
        }
        chunk_file = fopen(chunk_path, "rb");
        if (chunk_file == NULL) {
            mem_service_quarantine_chunked_payload_block(
                storage_root, record->object_payload_checksum);
            return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
        }
        for (;;) {
            size_t got = fread(buffer, 1U, sizeof(buffer), chunk_file);
            size_t i;

            if (got == 0U) {
                break;
            }
            for (i = 0U; i < got; ++i) {
                hash ^= buffer[i];
                hash *= 1099511628211ULL;
            }
            actual_len += (uint64_t)got;
        }
        if (fclose(chunk_file) != 0) {
            mem_service_quarantine_chunked_payload_block(
                storage_root, record->object_payload_checksum);
            return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
        }
    }
    if (actual_len != record->object_backing_len ||
        hash != record->object_payload_checksum ||
        manifest_total_checksum != record->object_payload_checksum) {
        mem_service_quarantine_chunked_payload_block(storage_root,
                                                     record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_validate_transport_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char dir_path[512];
    char manifest_path[512];
    char block_path[512];
    char line[160];
    uint8_t buffer[1024];
    uint64_t hash = 1469598103934665603ULL;
    uint64_t actual_len = 0U;
    uint64_t manifest_total_checksum = 0U;
    bool saw_backend = false;
    bool saw_transport = false;
    bool saw_total_checksum = false;
    FILE *manifest_file;
    FILE *payload_file;

    if (record == NULL ||
        record->object_payload_kind !=
            MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (storage_root == NULL || storage_root[0] == '\0' ||
        record->object_payload_checksum == 0U ||
        mem_service_make_transport_block_dir_path(storage_root,
                                                  record->object_payload_checksum,
                                                  dir_path,
                                                  sizeof(dir_path)) != 0 ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0) {
        mem_service_quarantine_transport_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    manifest_file = fopen(manifest_path, "r");
    if (manifest_file == NULL) {
        mem_service_quarantine_transport_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    while (fgets(line, sizeof(line), manifest_file) != NULL) {
        mem_service_trim_line(line);
        if (strcmp(line, "backend=transport-loopback-block-v1") == 0) {
            saw_backend = true;
        } else if (strcmp(line, "transport=file-copy-v1") == 0) {
            saw_transport = true;
        } else if (strncmp(line,
                           "total_checksum=0x",
                           sizeof("total_checksum=0x") - 1U) == 0) {
            manifest_total_checksum = (uint64_t)strtoull(
                line + sizeof("total_checksum=0x") - 1U, NULL, 16);
            saw_total_checksum = true;
        }
    }
    fclose(manifest_file);
    if (!saw_backend || !saw_transport || !saw_total_checksum ||
        manifest_total_checksum != record->object_payload_checksum) {
        mem_service_quarantine_transport_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    payload_file = fopen(block_path, "rb");
    if (payload_file == NULL) {
        mem_service_quarantine_transport_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    for (;;) {
        size_t got = fread(buffer, 1U, sizeof(buffer), payload_file);
        size_t i;

        for (i = 0U; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
        actual_len += (uint64_t)got;
        if (got < sizeof(buffer)) {
            if (ferror(payload_file)) {
                fclose(payload_file);
                mem_service_quarantine_transport_payload_block(
                    storage_root, record->object_payload_checksum);
                return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
            }
            break;
        }
    }
    fclose(payload_file);
    if (actual_len != record->object_backing_len ||
        hash != record->object_payload_checksum) {
        mem_service_quarantine_transport_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_validate_transport_tcp_payload_block(
    const char *storage_root,
    const struct mem_service_record *record)
{
    char dir_path[512];
    char manifest_path[512];
    char block_path[512];
    char line[160];
    uint8_t buffer[1024];
    uint64_t hash = 1469598103934665603ULL;
    uint64_t actual_len = 0U;
    uint64_t manifest_total_checksum = 0U;
    bool saw_backend = false;
    bool saw_transport = false;
    bool saw_total_checksum = false;
    FILE *manifest_file;
    FILE *payload_file;

    if (record == NULL ||
        record->object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (storage_root == NULL || storage_root[0] == '\0' ||
        record->object_payload_checksum == 0U ||
        mem_service_make_transport_tcp_block_dir_path(storage_root,
                                                      record->object_payload_checksum,
                                                      dir_path,
                                                      sizeof(dir_path)) != 0 ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0) {
        mem_service_quarantine_transport_tcp_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    manifest_file = fopen(manifest_path, "r");
    if (manifest_file == NULL) {
        mem_service_quarantine_transport_tcp_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    while (fgets(line, sizeof(line), manifest_file) != NULL) {
        mem_service_trim_line(line);
        if (strcmp(line, "backend=transport-tcp-block-v1") == 0) {
            saw_backend = true;
        } else if (strcmp(line, "transport=tcp-loopback-v1") == 0) {
            saw_transport = true;
        } else if (strncmp(line,
                           "total_checksum=0x",
                           sizeof("total_checksum=0x") - 1U) == 0) {
            manifest_total_checksum = (uint64_t)strtoull(
                line + sizeof("total_checksum=0x") - 1U, NULL, 16);
            saw_total_checksum = true;
        }
    }
    fclose(manifest_file);
    if (!saw_backend || !saw_transport || !saw_total_checksum ||
        manifest_total_checksum != record->object_payload_checksum) {
        mem_service_quarantine_transport_tcp_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    payload_file = fopen(block_path, "rb");
    if (payload_file == NULL) {
        mem_service_quarantine_transport_tcp_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    for (;;) {
        size_t got = fread(buffer, 1U, sizeof(buffer), payload_file);
        size_t i;

        for (i = 0U; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
        actual_len += (uint64_t)got;
        if (got < sizeof(buffer)) {
            if (ferror(payload_file)) {
                fclose(payload_file);
                mem_service_quarantine_transport_tcp_payload_block(
                    storage_root, record->object_payload_checksum);
                return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
            }
            break;
        }
    }
    fclose(payload_file);
    if (actual_len != record->object_backing_len ||
        hash != record->object_payload_checksum) {
        mem_service_quarantine_transport_tcp_payload_block(
            storage_root, record->object_payload_checksum);
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    return MEM_SERVICE_WIRE_STATUS_OK;
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
    if (mem_service_payload_get_u32(payload, "payload_kind", 0) ==
        MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK) {
        return mem_service_write_chunked_payload_block(storage_root,
                                                       payload,
                                                       payload_inline,
                                                       payload_path,
                                                       record);
    }
    if (mem_service_payload_get_u32(payload, "payload_kind", 0) ==
        MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK) {
        return mem_service_write_transport_payload_block(storage_root,
                                                         payload,
                                                         payload_inline,
                                                         payload_path,
                                                         record);
    }
    if (mem_service_payload_get_u32(payload, "payload_kind", 0) ==
        MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK) {
        return mem_service_write_transport_tcp_payload_block(storage_root,
                                                            payload,
                                                            payload_inline,
                                                            payload_path,
                                                            record);
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

    if (record == NULL) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (record->object_payload_kind ==
        MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK) {
        return mem_service_validate_chunked_payload_block(storage_root, record);
    }
    if (record->object_payload_kind ==
        MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK) {
        return mem_service_validate_transport_payload_block(storage_root, record);
    }
    if (record->object_payload_kind ==
        MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK) {
        return mem_service_validate_transport_tcp_payload_block(storage_root, record);
    }
    if (record->object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_SEALED_LOCAL_BLOCK) {
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

static int mem_service_load_store(struct mem_service *svc,
                                  const char *store_path,
                                  bool *legacy_schema_out)
{
    FILE *file;
    char line[512];
    struct mem_service_store_import_state state;
    bool saw_schema_version = false;

    if (legacy_schema_out != NULL) {
        *legacy_schema_out = false;
    }
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
        if (!state.in_record && !state.in_idempotency && !state.in_audit) {
            int schema_rc = mem_service_parse_schema_version_line(
                line,
                "store_schema_version=",
                MEM_SERVICE_STORE_MAX_KNOWN_SCHEMA_VERSION,
                &saw_schema_version);

            if (schema_rc != 0) {
                if (schema_rc < 0) {
                    fclose(file);
                    return -1;
                }
                continue;
            }
        }
        if (mem_service_import_store_line(svc, line, &state) != 0) {
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    if (legacy_schema_out != NULL && !saw_schema_version) {
        *legacy_schema_out = true;
    }
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
    /* A frame left open at EOF (state.in_record/in_idempotency/in_audit) is a
     * torn trailing record from a crash mid-append. It was never closed, so it
     * was never committed into svc; drop it and recover the complete records
     * already parsed, rather than bricking restart. Mid-field parse errors
     * inside the loop above still fail-closed. */
    return 0;
}

static int mem_service_load_durable_store(struct mem_service *svc,
                                          const char *store_path)
{
    bool legacy_schema = false;

    if (mem_service_load_store(svc, store_path, &legacy_schema) != 0) {
        return -1;
    }
    if (mem_service_load_journal(svc, store_path) != 0) {
        return -1;
    }
    if (legacy_schema &&
        (mem_service_save_store(svc, store_path) != 0 ||
         mem_service_compact_journal(store_path) != 0)) {
        return -1;
    }
    return 0;
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
    if (record->object_backend_kind != MEM_SERVICE_OBJECT_BACKEND_LEGACY_PAYLOAD &&
        fprintf(file,
                "object_backend_kind=%u\n"
                "object_backend_node=%u\n"
                "object_backend_device_cna=%u\n"
                "object_backend_flags=%u\n"
                "object_backend_block_hi=%" PRIu64 "\n"
                "object_backend_block_lo=%" PRIu64 "\n"
                "object_backend_block_version=%" PRIu64 "\n"
                "object_backend_block_offset=%" PRIu64 "\n"
                "object_backend_block_bytes=%" PRIu64 "\n"
                "object_backend_block_checksum=%" PRIu64 "\n",
                record->object_backend_kind,
                record->object_backend_node,
                record->object_backend_device_cna,
                record->object_backend_flags,
                record->object_backend_block_hi,
                record->object_backend_block_lo,
                record->object_backend_block_version,
                record->object_backend_block_offset,
                record->object_backend_block_bytes,
                record->object_backend_block_checksum) < 0) {
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
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
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
                "store_schema_version=%d\n"
                "record_count=%zu\n"
                "audit_next_sequence=%" PRIu64 "\n"
                "audit_event_count=%" PRIu64 "\n",
                MEM_SERVICE_STORE_MAGIC,
                MEM_SERVICE_STORE_SCHEMA_VERSION,
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

static int mem_service_count_lines_containing(const char *path,
                                            const char *needle,
                                            uint64_t *count_out)
{
    FILE *file;
    char line[512];
    uint64_t count = 0;

    if (path == NULL || needle == NULL || count_out == NULL) {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            count += 1U;
        }
    }
    fclose(file);
    *count_out = count;
    return 0;
}

static int mem_service_rewrite_journal_header(const char *journal_path)
{
    char tmp_path[512];
    FILE *file;

    if (journal_path == NULL || journal_path[0] == '\0') {
        return -1;
    }
    if (snprintf(tmp_path,
                 sizeof(tmp_path),
                 "%s.compact.%ld",
                 journal_path,
                 (long)getpid()) >= (int)sizeof(tmp_path)) {
        return -1;
    }
    file = fopen(tmp_path, "w");
    if (file == NULL) {
        return -1;
    }
    if (fprintf(file, "%s\n", MEM_SERVICE_JOURNAL_MAGIC) < 0 || fflush(file) != 0 ||
        fsync(fileno(file)) != 0 || fclose(file) != 0) {
        fclose(file);
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, journal_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int mem_service_compact_journal(const char *store_path)
{
    char journal_path[512];
    struct stat st;

    if (store_path == NULL || store_path[0] == '\0') {
        return 0;
    }
    if (mem_service_make_journal_path(store_path, journal_path, sizeof(journal_path)) != 0) {
        return 0;
    }
    if (stat(journal_path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if ((uint64_t)st.st_size <= MEM_SERVICE_JOURNAL_COMPACTION_THRESHOLD_BYTES) {
        return 0;
    }
    return mem_service_rewrite_journal_header(journal_path);
}

static int mem_service_compact_journal_now(const char *store_path)
{
    char journal_path[512];

    if (store_path == NULL || store_path[0] == '\0') {
        return 0;
    }
    if (mem_service_make_journal_path(store_path, journal_path, sizeof(journal_path)) != 0) {
        return 0;
    }
    return mem_service_rewrite_journal_header(journal_path);
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
    if (!mem_service_file_contains(store_path, "store_schema_version=1\n")) {
        fprintf(stderr, "mem_service store-fixtures: store schema missing\n");
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
    {
        struct mem_service migrated;
        struct mem_service_record legacy_record;
        FILE *legacy_file = fopen(store_path, "w");

        unlink(journal_path);
        if (legacy_file == NULL ||
            fprintf(legacy_file,
                    "%s\n"
                    "record_count=1\n"
                    "audit_next_sequence=1\n"
                    "audit_event_count=0\n"
                    "record_begin\n"
                    "kind=%u\n"
                    "key=legacy-store-object\n"
                    "version=11\n"
                    "record_end\n",
                    MEM_SERVICE_STORE_MAGIC,
                    (uint32_t)MEM_SERVICE_RECORD_KVCACHE_OBJECT) < 0 ||
            fclose(legacy_file) != 0) {
            if (legacy_file != NULL) {
                fclose(legacy_file);
            }
            fprintf(stderr, "mem_service store-fixtures: legacy store write failed\n");
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
        if (mem_service_init(&migrated, true, true, true) != 0 ||
            mem_service_load_durable_store(&migrated, store_path) != 0 ||
            mem_service_get_record(&migrated,
                                   "legacy-store-object",
                                   &legacy_record) != 0 ||
            legacy_record.version != 11 ||
            !mem_service_file_contains(store_path, "store_schema_version=1\n")) {
            fprintf(stderr,
                    "mem_service store-fixtures: legacy store migration failed\n");
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    {
        struct mem_service rejected;
        FILE *future_file = fopen(store_path, "w");

        unlink(journal_path);
        if (future_file == NULL ||
            fprintf(future_file,
                    "%s\n"
                    "store_schema_version=99\n"
                    "record_count=0\n"
                    "audit_next_sequence=1\n"
                    "audit_event_count=0\n",
                    MEM_SERVICE_STORE_MAGIC) < 0 ||
            fclose(future_file) != 0) {
            if (future_file != NULL) {
                fclose(future_file);
            }
            fprintf(stderr, "mem_service store-fixtures: future store write failed\n");
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
        if (mem_service_init(&rejected, true, true, true) != 0 ||
            mem_service_load_durable_store(&rejected, store_path) == 0) {
            fprintf(stderr,
                    "mem_service store-fixtures: future store schema not refused\n");
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    {
        struct mem_service rejected;
        FILE *malformed_file = fopen(store_path, "w");

        unlink(journal_path);
        if (malformed_file == NULL ||
            fprintf(malformed_file,
                    "%s\n"
                    "store_schema_version=1abc\n"
                    "record_count=0\n"
                    "audit_next_sequence=1\n"
                    "audit_event_count=0\n",
                    MEM_SERVICE_STORE_MAGIC) < 0 ||
            fclose(malformed_file) != 0) {
            if (malformed_file != NULL) {
                fclose(malformed_file);
            }
            fprintf(stderr,
                    "mem_service store-fixtures: malformed store write failed\n");
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
        if (mem_service_init(&rejected, true, true, true) != 0 ||
            mem_service_load_durable_store(&rejected, store_path) == 0) {
            fprintf(stderr,
                    "mem_service store-fixtures: malformed store schema not refused\n");
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service store-fixtures: status=ok records=%zu key=%s version=%" PRIu64
           " checksum=%" PRIu64 " idempotency_replay=%" PRIu64
           " journal_events=%" PRIu64
           " store_schema_version=%d store_migration=legacy-to-v1\n",
           second.record_count,
           record.key,
           record.version,
           record.object_payload_checksum,
           second.metrics.idempotency_replay_count,
           second.audit_event_count,
           MEM_SERVICE_STORE_SCHEMA_VERSION);
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

int mem_service_run_journal_torn_recovery_fixture_check(void)
{
    static const char payload[] =
        "key=journal-torn-object\n"
        "version=17\n"
        "checksum=17017\n"
        "backing_len=96\n"
        "idempotency_key=journal-torn-idem\n";
    struct mem_service writer;
    struct mem_service recovery;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char replay_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[160];
    char journal_path[sizeof(store_path) + 16U];
    enum mem_service_wire_status status;
    enum mem_service_wire_status replay_status;
    FILE *file;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_journal_torn_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    if (mem_service_init(&writer, true, true, true) != 0 ||
        mem_service_init(&recovery, true, true, true) != 0) {
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    /* Write one complete idempotency + audit frame through the real path
     * (append_journal now fsyncs each record). */
    status = mem_service_handle_operation(&writer,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          payload,
                                          response,
                                          sizeof(response),
                                          store_path,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: put failed status=%s\n",
                mem_service_wire_status_name(status));
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (!mem_service_file_contains(journal_path, "idempotency_end\n")) {
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: complete frame missing\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    /* Simulate a crash mid-append: raw-append a torn trailing idempotency
     * frame that opens but never closes (no idempotency_end). */
    file = fopen(journal_path, "a");
    if (file == NULL ||
        fprintf(file,
                "idempotency_begin\n"
                "key=journal-torn-victim\n") < 0 ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: torn append failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    /* The torn trailing frame must NOT brick recovery: load_journal drops the
     * incomplete tail and returns 0, and the complete prior frame is
     * replayable. */
    if (mem_service_load_journal(&recovery, store_path) != 0) {
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: torn load failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    replay_status = mem_service_handle_operation(&recovery,
                                                 MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                 payload,
                                                 replay_response,
                                                 sizeof(replay_response),
                                                 NULL,
                                                 NULL);
    unlink(store_path);
    unlink(journal_path);
    if (replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strcmp(response, replay_response) != 0 ||
        recovery.metrics.idempotency_replay_count != 1U) {
        fprintf(stderr,
                "mem_service journal-torn-recovery-fixtures: replay recovery "
                "mismatch replay=%u idempotency_replay=%" PRIu64 "\n",
                replay_status,
                recovery.metrics.idempotency_replay_count);
        return 1;
    }
    printf("mem_service journal-torn-recovery-fixtures: status=ok "
           "torn_recovery=ok journal_magic=%s idempotency_replay=%" PRIu64
           " atomic_append_barrier=fsync\n",
           MEM_SERVICE_JOURNAL_MAGIC,
           recovery.metrics.idempotency_replay_count);
    return 0;
}

int mem_service_run_journal_compaction_fixture_check(void)
{
    static const char base_payload[] =
        "key=journal-compaction-base-object\n"
        "version=1\n"
        "checksum=7001\n"
        "backing_len=64\n"
        "idempotency_key=journal-compaction-base-idem\n";
    struct mem_service writer;
    struct mem_service recovery;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char base_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char replay_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char payload[256];
    char store_path[192];
    char journal_path[208];
    char key[48];
    struct stat journal_stat;
    enum mem_service_wire_status status;
    enum mem_service_wire_status replay_status;
    struct mem_service_record record;
    uint64_t matching_key_hits = 0;
    size_t i;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_compact_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    if (mem_service_init(&writer, true, true, true) != 0 ||
        mem_service_init(&recovery, true, true, true) != 0) {
        fprintf(stderr, "mem_service journal-compaction-fixtures: init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    status = mem_service_handle_operation(&writer,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          base_payload,
                                          response,
                                          sizeof(response),
                                          store_path,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: base put failed status=%s\n",
                mem_service_wire_status_name(status));
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    snprintf(base_response, sizeof(base_response), "%s", response);
    for (i = 0; i < 64U; ++i) {
        snprintf(key, sizeof(key), "journal-compaction-object-%zu", i);
        snprintf(payload,
                 sizeof(payload),
                 "key=%s\n"
                 "version=%zu\n"
                 "checksum=%zu\n"
                 "backing_len=%zu\n",
                 key,
                 i + 2U,
                 i + 9000U,
                 i + 96U);
        status = mem_service_handle_operation(&writer,
                                            MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                            payload,
                                            response,
                                            sizeof(response),
                                            store_path,
                                            NULL);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service journal-compaction-fixtures: burst put failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name(status));
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    if (stat(journal_path, &journal_stat) != 0) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: journal stat failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if ((uint64_t)journal_stat.st_size >
        MEM_SERVICE_JOURNAL_COMPACTION_THRESHOLD_BYTES) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: compact failed, size=%" PRIu64 "\n",
                (uint64_t)journal_stat.st_size);
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_count_lines_containing(journal_path,
                                          "key=journal-compaction-object-",
                                          &matching_key_hits) != 0 ||
        matching_key_hits >= 64U) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: compaction too little\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_load_durable_store(&recovery, store_path) != 0) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: durable load failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    replay_status = mem_service_handle_operation(&recovery,
                                                MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                                base_payload,
                                                replay_response,
                                                sizeof(replay_response),
                                                NULL,
                                                NULL);
    if (mem_service_get_record(&recovery,
                              "journal-compaction-base-object",
                              &record) != 0 ||
        record.version != 1U) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: compact recovery record mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (replay_status != MEM_SERVICE_WIRE_STATUS_OK ||
        recovery.metrics.idempotency_replay_count != 1U ||
        strcmp(base_response, replay_response) != 0) {
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: compact recovery mismatch "
                "replay_status=%s idempotency_replay=%" PRIu64
                " response_match=%d\n",
                mem_service_wire_status_name(replay_status),
                recovery.metrics.idempotency_replay_count,
                strcmp(base_response, replay_response) == 0);
        fprintf(stderr,
                "mem_service journal-compaction-fixtures: base_response=%s\n",
                base_response);
        fprintf(stderr, "mem_service journal-compaction-fixtures: replay_response=%s\n", replay_response);
        fprintf(stderr, "mem_service journal-compaction-fixtures: record_key=%s\n", record.key);
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service journal-compaction-fixtures: status=ok journal_compaction=1 "
           "journal_magic=%s journal_size=%" PRIu64 " compact_threshold=%u "
           "idempotency_replay=%" PRIu64 " matching_dynamic_keys=%" PRIu64 "\n",
           MEM_SERVICE_JOURNAL_MAGIC,
           (uint64_t)journal_stat.st_size,
           MEM_SERVICE_JOURNAL_COMPACTION_THRESHOLD_BYTES,
           recovery.metrics.idempotency_replay_count,
           matching_key_hits);
    return 0;
}

int mem_service_run_restore_policy_fixture_check(void)
{
    static const char anchor_payload[] =
        "key=restore-policy-anchor\n"
        "version=1\n"
        "checksum=1001\n"
        "backing_len=64\n"
        "idempotency_key=restore-policy-anchor-v1\n";
    static const char bad_magic_snapshot[] =
        "not_mem_service_store\n"
        "record_count=0\n"
        "audit_next_sequence=1\n"
        "audit_event_count=0\n";
    static const char bad_future_schema_snapshot[] =
        "mem_service_store_v1\n"
        "store_schema_version=99\n"
        "record_count=0\n"
        "audit_next_sequence=1\n"
        "audit_event_count=0\n";
    static const char bad_malformed_schema_snapshot[] =
        "mem_service_store_v1\n"
        "store_schema_version=1abc\n"
        "record_count=0\n"
        "audit_next_sequence=1\n"
        "audit_event_count=0\n";
    static const char begin_one_payload[] =
        "action=begin\n"
        "expected_records=1\n";
    static const char begin_two_payload[] =
        "action=begin\n"
        "expected_records=2\n";
    static const char wrong_page_payload[] =
        "action=append\n"
        "page_index=1\n"
        "complete=1\n";
    static const char commit_payload[] =
        "action=commit\n";
    static const char cancel_payload[] =
        "action=cancel\n";
    static const char page_request[] =
        "start_index=0\n"
        "max_records=1\n";
    struct mem_service svc;
    struct mem_service full_restored;
    struct mem_service restored;
    struct mem_service_record record;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char snapshot[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char page[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char append_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN + 64U];
    enum mem_service_wire_status status;
    enum mem_service_wire_status bad_full_status;
    enum mem_service_wire_status bad_future_schema_status;
    enum mem_service_wire_status bad_malformed_schema_status;
    enum mem_service_wire_status wrong_page_status;
    enum mem_service_wire_status mismatch_commit_status;
    enum mem_service_wire_status cancelled_commit_status;
    enum mem_service_wire_status good_commit_status;
    int failures = 0;

    if (mem_service_init(&svc, true, true, true) != 0 ||
        mem_service_init(&full_restored, true, true, true) != 0 ||
        mem_service_init(&restored, true, true, true) != 0) {
        fprintf(stderr, "mem_service restore-policy-fixtures: init failed\n");
        return 1;
    }
    status = mem_service_handle_operation(&svc,
                                          MEM_SERVICE_WIRE_OP_PUT_OBJECT,
                                          anchor_payload,
                                          response,
                                          sizeof(response),
                                          NULL,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: anchor put failed status=%s\n",
                mem_service_wire_status_name(status));
        return 1;
    }
    status = mem_service_handle_operation(&svc,
                                          MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                          "",
                                          snapshot,
                                          sizeof(snapshot),
                                          NULL,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(snapshot, MEM_SERVICE_STORE_MAGIC) == NULL ||
        strstr(snapshot, "key=restore-policy-anchor\n") == NULL) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: full snapshot export failed\n");
        return 1;
    }
    status = mem_service_handle_operation(&svc,
                                          MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                          page_request,
                                          page,
                                          sizeof(page),
                                          NULL,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(page, "snapshot_page=1\n") == NULL ||
        strstr(page, "key=restore-policy-anchor\n") == NULL) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: paged snapshot export failed\n");
        return 1;
    }
    status = mem_service_handle_operation(&full_restored,
                                          MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                          snapshot,
                                          response,
                                          sizeof(response),
                                          NULL,
                                          NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        mem_service_get_record(&full_restored, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: full snapshot restore failed\n");
        return 1;
    }

    bad_full_status = mem_service_handle_operation(&svc,
                                                   MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                                   bad_magic_snapshot,
                                                   response,
                                                   sizeof(response),
                                                   NULL,
                                                   NULL);
    if (bad_full_status != MEM_SERVICE_WIRE_STATUS_INVALID_SESSION ||
        mem_service_get_record(&svc, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: bad full restore polluted live state\n");
        failures -= 1;
    }
    bad_future_schema_status =
        mem_service_handle_operation(&svc,
                                     MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                     bad_future_schema_snapshot,
                                     response,
                                     sizeof(response),
                                     NULL,
                                     NULL);
    bad_malformed_schema_status =
        mem_service_handle_operation(&svc,
                                     MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                     bad_malformed_schema_snapshot,
                                     response,
                                     sizeof(response),
                                     NULL,
                                     NULL);
    if (bad_future_schema_status != MEM_SERVICE_WIRE_STATUS_INVALID_SESSION ||
        bad_malformed_schema_status != MEM_SERVICE_WIRE_STATUS_INVALID_SESSION ||
        mem_service_get_record(&svc, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: schema version restore "
                "did not fail closed\n");
        failures -= 1;
    }

    status = mem_service_handle_operation(&svc,
                                          MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                          begin_one_payload,
                                          response,
                                          sizeof(response),
                                          NULL,
                                          NULL);
    wrong_page_status = mem_service_handle_operation(&svc,
                                                     MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                                     wrong_page_payload,
                                                     response,
                                                     sizeof(response),
                                                     NULL,
                                                     NULL);
    (void)mem_service_handle_operation(&svc,
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       cancel_payload,
                                       response,
                                       sizeof(response),
                                       NULL,
                                       NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        wrong_page_status != MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT ||
        mem_service_get_record(&svc, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: out-of-order page did not fail closed\n");
        failures -= 1;
    }

    int append_payload_len = snprintf(append_payload,
                                      sizeof(append_payload),
                                      "action=append\npage_index=0\n%s",
                                      page);
    if (append_payload_len < 0 ||
        (size_t)append_payload_len >= sizeof(append_payload)) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: append payload truncated\n");
        return 1;
    }
    status = mem_service_handle_operation(&svc,
                                          MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                          begin_two_payload,
                                          response,
                                          sizeof(response),
                                          NULL,
                                          NULL);
    if (status == MEM_SERVICE_WIRE_STATUS_OK) {
        status = mem_service_handle_operation(&svc,
                                              MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                              append_payload,
                                              response,
                                              sizeof(response),
                                              NULL,
                                              NULL);
    }
    mismatch_commit_status =
        mem_service_handle_operation(&svc,
                                     MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                     commit_payload,
                                     response,
                                     sizeof(response),
                                     NULL,
                                     NULL);
    (void)mem_service_handle_operation(&svc,
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       cancel_payload,
                                       response,
                                       sizeof(response),
                                       NULL,
                                       NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        mismatch_commit_status != MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT ||
        mem_service_get_record(&svc, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: count mismatch did not fail closed\n");
        failures -= 1;
    }

    (void)mem_service_handle_operation(&svc,
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       begin_one_payload,
                                       response,
                                       sizeof(response),
                                       NULL,
                                       NULL);
    (void)mem_service_handle_operation(&svc,
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       cancel_payload,
                                       response,
                                       sizeof(response),
                                       NULL,
                                       NULL);
    cancelled_commit_status =
        mem_service_handle_operation(&svc,
                                     MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                     commit_payload,
                                     response,
                                     sizeof(response),
                                     NULL,
                                     NULL);
    if (cancelled_commit_status != MEM_SERVICE_WIRE_STATUS_INVALID_SESSION ||
        mem_service_get_record(&svc, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: cancelled stage did not fail closed\n");
        failures -= 1;
    }

    status = mem_service_handle_operation(&restored,
                                          MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                          begin_one_payload,
                                          response,
                                          sizeof(response),
                                          NULL,
                                          NULL);
    if (status == MEM_SERVICE_WIRE_STATUS_OK) {
        status = mem_service_handle_operation(&restored,
                                              MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                              append_payload,
                                              response,
                                              sizeof(response),
                                              NULL,
                                              NULL);
    }
    good_commit_status = mem_service_handle_operation(&restored,
                                                      MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                                      commit_payload,
                                                      response,
                                                      sizeof(response),
                                                      NULL,
                                                      NULL);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        good_commit_status != MEM_SERVICE_WIRE_STATUS_OK ||
        mem_service_get_record(&restored, "restore-policy-anchor", &record) != 0 ||
        record.version != 1U || record.object_payload_checksum != 1001U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: staged commit restore failed\n");
        failures -= 1;
    }
    if (svc.metrics.invalid_session_count != 4U ||
        svc.metrics.version_conflict_count != 2U ||
        svc.metrics.fail_closed_count != 6U) {
        fprintf(stderr,
                "mem_service restore-policy-fixtures: counter mismatch "
                "invalid_session=%" PRIu64 " version_conflict=%" PRIu64
                " fail_closed=%" PRIu64 "\n",
                svc.metrics.invalid_session_count,
                svc.metrics.version_conflict_count,
                svc.metrics.fail_closed_count);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service restore-policy-fixtures: status=ok "
           "restore_policy=transactional-staged-restore "
           "restore_scope=full-snapshot,paged-snapshot "
           "full_restore=ok paged_restore=ok "
           "fail_closed_cases=bad-magic,future-store-schema,malformed-store-schema,out-of-order-page,record-count-mismatch,cancelled-stage-commit "
           "live_state=unchanged-until-commit "
           "invalid_session=%" PRIu64 " version_conflict=%" PRIu64
           " fail_closed=%" PRIu64 "\n",
           svc.metrics.invalid_session_count,
           svc.metrics.version_conflict_count,
           svc.metrics.fail_closed_count);
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
           " old_server_runtime_binary=in-tree\n",
           server.metrics.idempotency_replay_count,
           server.metrics.idempotency_conflict_count,
           server.metrics.fail_closed_count);
    return 0;
}

int mem_service_run_compat_old_server_runtime_fixture_check(void)
{
    static const char publish_payload[] =
        "key=runtime/compat-oldsrv/cert/session-a/range-0\n"
        "session_id=cert-session-a\n"
        "model_key=cert-model\n"
        "artifact_kind=hidden-range\n"
        "artifact_id=range-0\n"
        "checksum=9009\n"
        "version=9\n"
        "idempotency_key=compat-oldsrv-publish-range-0-v9\n";
    static const char matching_query[] =
        "key=runtime/compat-oldsrv/cert/session-a/range-0\n"
        "expected_session_id=cert-session-a\n"
        "expected_model_key=cert-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=9\n"
        "expected_checksum=9009\n";
    static const char bad_session_query[] =
        "key=runtime/compat-oldsrv/cert/session-a/range-0\n"
        "expected_session_id=wrong-session\n"
        "expected_model_key=cert-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=9\n"
        "expected_checksum=9009\n";
    static const char bad_model_query[] =
        "key=runtime/compat-oldsrv/cert/session-a/range-0\n"
        "expected_session_id=cert-session-a\n"
        "expected_model_key=wrong-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=9\n"
        "expected_checksum=9009\n";
    static const char stale_version_query[] =
        "key=runtime/compat-oldsrv/cert/session-a/range-0\n"
        "expected_session_id=cert-session-a\n"
        "expected_model_key=cert-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=10\n"
        "expected_checksum=9009\n";
    static const char checksum_mismatch_query[] =
        "key=runtime/compat-oldsrv/cert/session-a/range-0\n"
        "expected_session_id=cert-session-a\n"
        "expected_model_key=cert-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_version=9\n"
        "expected_checksum=9010\n";
    static struct mem_service current_server;
    static struct mem_service old_server;
    char publish_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char matching_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char adversarial_response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status current_publish_status;
    enum mem_service_wire_status old_publish_status;
    enum mem_service_wire_status current_matching_status;
    enum mem_service_wire_status old_matching_status;
    enum mem_service_wire_status current_bad_session_status;
    enum mem_service_wire_status current_bad_model_status;
    enum mem_service_wire_status current_stale_status;
    enum mem_service_wire_status current_checksum_status;
    enum mem_service_wire_status old_bad_session_status;
    enum mem_service_wire_status old_bad_model_status;
    enum mem_service_wire_status old_stale_status;
    enum mem_service_wire_status old_checksum_status;
    int failures = 0;

    if (mem_service_init(&current_server, true, true, true) != 0 ||
        mem_service_init(&old_server, true, true, true) != 0) {
        fprintf(stderr,
                "mem_service compat-old-server-runtime-fixtures: init failed\n");
        return 1;
    }
    old_server.enforce_expected_context = false;

    current_publish_status =
        mem_service_handle_operation(&current_server,
                                     MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                     publish_payload,
                                     publish_response,
                                     sizeof(publish_response),
                                     NULL,
                                     NULL);
    old_publish_status =
        mem_service_handle_operation(&old_server,
                                     MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                     publish_payload,
                                     publish_response,
                                     sizeof(publish_response),
                                     NULL,
                                     NULL);
    if (current_publish_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_publish_status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service compat-old-server-runtime-fixtures: publish path mismatch\n");
        failures -= 1;
    }

    current_matching_status =
        mem_service_handle_operation(&current_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     matching_query,
                                     matching_response,
                                     sizeof(matching_response),
                                     NULL,
                                     NULL);
    old_matching_status =
        mem_service_handle_operation(&old_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     matching_query,
                                     matching_response,
                                     sizeof(matching_response),
                                     NULL,
                                     NULL);
    if (current_matching_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_matching_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(matching_response, "version=9\n") == NULL ||
        strstr(matching_response, "object_payload_checksum=9009\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-old-server-runtime-fixtures: matching query mismatch\n");
        failures -= 1;
    }

    current_bad_session_status =
        mem_service_handle_operation(&current_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     bad_session_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    current_bad_model_status =
        mem_service_handle_operation(&current_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     bad_model_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    current_stale_status =
        mem_service_handle_operation(&current_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     stale_version_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    current_checksum_status =
        mem_service_handle_operation(&current_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     checksum_mismatch_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    old_bad_session_status =
        mem_service_handle_operation(&old_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     bad_session_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    old_bad_model_status =
        mem_service_handle_operation(&old_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     bad_model_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    old_stale_status =
        mem_service_handle_operation(&old_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     stale_version_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    old_checksum_status =
        mem_service_handle_operation(&old_server,
                                     MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                     checksum_mismatch_query,
                                     adversarial_response,
                                     sizeof(adversarial_response),
                                     NULL,
                                     NULL);
    if (current_bad_session_status != MEM_SERVICE_WIRE_STATUS_INVALID_SESSION ||
        current_bad_model_status != MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING ||
        current_stale_status != MEM_SERVICE_WIRE_STATUS_STALE_REF ||
        current_checksum_status != MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH) {
        fprintf(stderr,
                "mem_service compat-old-server-runtime-fixtures: current server "
                "fail-closed path mismatch (session=%u model=%u stale=%u checksum=%u)\n",
                current_bad_session_status,
                current_bad_model_status,
                current_stale_status,
                current_checksum_status);
        failures -= 1;
    }
    if (old_bad_session_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_bad_model_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_stale_status != MEM_SERVICE_WIRE_STATUS_OK ||
        old_checksum_status != MEM_SERVICE_WIRE_STATUS_OK ||
        strstr(adversarial_response, "version=9\n") == NULL ||
        strstr(adversarial_response, "object_payload_checksum=9009\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-old-server-runtime-fixtures: old server "
                "tolerant path mismatch (session=%u model=%u stale=%u checksum=%u)\n",
                old_bad_session_status,
                old_bad_model_status,
                old_stale_status,
                old_checksum_status);
        failures -= 1;
    }

    if (current_server.metrics.fail_closed_count != 4U ||
        current_server.metrics.invalid_session_count != 1U ||
        current_server.metrics.invalid_model_binding_count != 1U ||
        current_server.metrics.stale_ref_count != 1U ||
        current_server.metrics.checksum_mismatch_count != 1U ||
        old_server.metrics.fail_closed_count != 0U ||
        old_server.metrics.invalid_session_count != 0U ||
        old_server.metrics.invalid_model_binding_count != 0U ||
        old_server.metrics.stale_ref_count != 0U ||
        old_server.metrics.checksum_mismatch_count != 0U) {
        fprintf(stderr,
                "mem_service compat-old-server-runtime-fixtures: metrics contrast "
                "mismatch current_fail_closed=%" PRIu64 " old_fail_closed=%" PRIu64
                " current_invalid_session=%" PRIu64
                " old_invalid_session=%" PRIu64 "\n",
                current_server.metrics.fail_closed_count,
                old_server.metrics.fail_closed_count,
                current_server.metrics.invalid_session_count,
                old_server.metrics.invalid_session_count);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }

    printf("mem_service compat-old-server-runtime-fixtures: status=ok "
           "new_client_old_server=certified "
           "old_server_runtime_binary=in-tree "
           "current_fail_closed=%" PRIu64 " old_fail_closed=%" PRIu64
           " current_invalid_model_binding=%" PRIu64
           " current_stale_ref=%" PRIu64
           " current_checksum_mismatch=%" PRIu64
           " old_served_adversarial=4\n",
           current_server.metrics.fail_closed_count,
           old_server.metrics.fail_closed_count,
           current_server.metrics.invalid_model_binding_count,
           current_server.metrics.stale_ref_count,
           current_server.metrics.checksum_mismatch_count);
    return 0;
}

int mem_service_run_serving_fail_closed_fixture_check(void)
{
    static const char runtime_publish[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "session_id=srv-session\n"
        "model_key=srv-model\n"
        "artifact_kind=hidden-range\n"
        "artifact_id=range-0\n"
        "owner=1\n"
        "checksum=7007\n"
        "version=7\n"
        "idempotency_key=serving-runtime-range-0-v7\n";
    static const char runtime_match[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=1\n"
        "expected_version=7\n"
        "expected_checksum=7007\n";
    static const char runtime_bad_session[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=wrong-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=1\n"
        "expected_version=7\n"
        "expected_checksum=7007\n";
    static const char runtime_bad_model[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=wrong-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=1\n"
        "expected_version=7\n"
        "expected_checksum=7007\n";
    static const char runtime_bad_kind[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=wrong-kind\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=1\n"
        "expected_version=7\n"
        "expected_checksum=7007\n";
    static const char runtime_bad_owner[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=99\n"
        "expected_version=7\n"
        "expected_checksum=7007\n";
    static const char runtime_bad_version[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=1\n"
        "expected_version=99\n"
        "expected_checksum=7007\n";
    static const char runtime_bad_checksum[] =
        "key=runtime/serving-matrix/session-a/range-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=hidden-range\n"
        "expected_artifact_id=range-0\n"
        "expected_owner=1\n"
        "expected_version=7\n"
        "expected_checksum=7008\n";
    static const char exec_register[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "session_id=srv-session\n"
        "request_id=srv-req-0\n"
        "model_key=srv-model\n"
        "artifact_kind=logits\n"
        "artifact_id=logits-0\n"
        "owner=2\n"
        "payload_kind=3\n"
        "backing_offset=64\n"
        "backing_len=256\n"
        "checksum=8008\n"
        "version=8\n"
        "idempotency_key=serving-exec-logits-0-v8\n";
    static const char exec_match[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=logits\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=2\n"
        "expected_version=8\n"
        "expected_checksum=8008\n";
    static const char exec_bad_session[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=wrong-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=logits\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=2\n"
        "expected_version=8\n"
        "expected_checksum=8008\n";
    static const char exec_bad_model[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=wrong-model\n"
        "expected_artifact_kind=logits\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=2\n"
        "expected_version=8\n"
        "expected_checksum=8008\n";
    static const char exec_bad_kind[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=wrong-kind\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=2\n"
        "expected_version=8\n"
        "expected_checksum=8008\n";
    static const char exec_bad_owner[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=logits\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=99\n"
        "expected_version=8\n"
        "expected_checksum=8008\n";
    static const char exec_bad_version[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=logits\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=2\n"
        "expected_version=99\n"
        "expected_checksum=8008\n";
    static const char exec_bad_checksum[] =
        "key=execution/serving-matrix/session-a/logits-0\n"
        "expected_session_id=srv-session\n"
        "expected_model_key=srv-model\n"
        "expected_artifact_kind=logits\n"
        "expected_artifact_id=logits-0\n"
        "expected_owner=2\n"
        "expected_version=8\n"
        "expected_checksum=8009\n";
    static struct mem_service svc;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status s;
    int failures = 0;

    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service serving-fail-closed-fixtures: init failed\n");
        return 1;
    }
#define SERVING_QUERY(op, q, expect)                                          \
    do {                                                                       \
        s = mem_service_handle_operation(&svc, (op), (q), response,            \
                                         sizeof(response), NULL, NULL);        \
        if (s != (expect)) {                                                   \
            fprintf(stderr,                                                    \
                    "mem_service serving-fail-closed-fixtures: " #op           \
                    " " #q " status=%u expected=%u\n",                         \
                    s, (expect));                                              \
            failures -= 1;                                                     \
        }                                                                      \
    } while (0)

    SERVING_QUERY(MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF, runtime_publish,
                  MEM_SERVICE_WIRE_STATUS_OK);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_match,
                  MEM_SERVICE_WIRE_STATUS_OK);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_bad_session,
                  MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_bad_model,
                  MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_bad_kind,
                  MEM_SERVICE_WIRE_STATUS_STALE_REF);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_bad_owner,
                  MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_bad_version,
                  MEM_SERVICE_WIRE_STATUS_STALE_REF);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF, runtime_bad_checksum,
                  MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH);

    SERVING_QUERY(MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT, exec_register,
                  MEM_SERVICE_WIRE_STATUS_OK);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_match,
                  MEM_SERVICE_WIRE_STATUS_OK);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_bad_session,
                  MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_bad_model,
                  MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_bad_kind,
                  MEM_SERVICE_WIRE_STATUS_STALE_REF);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_bad_owner,
                  MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_bad_version,
                  MEM_SERVICE_WIRE_STATUS_STALE_REF);
    SERVING_QUERY(MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT, exec_bad_checksum,
                  MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH);
#undef SERVING_QUERY

    if (svc.metrics.invalid_session_count != 2U ||
        svc.metrics.invalid_model_binding_count != 4U ||
        svc.metrics.stale_ref_count != 4U ||
        svc.metrics.checksum_mismatch_count != 2U ||
        svc.metrics.fail_closed_count != 12U) {
        fprintf(stderr,
                "mem_service serving-fail-closed-fixtures: counter mismatch "
                "invalid_session=%" PRIu64 " invalid_model=%" PRIu64
                " stale_ref=%" PRIu64 " checksum=%" PRIu64 " fail_closed=%" PRIu64 "\n",
                svc.metrics.invalid_session_count,
                svc.metrics.invalid_model_binding_count,
                svc.metrics.stale_ref_count,
                svc.metrics.checksum_mismatch_count,
                svc.metrics.fail_closed_count);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service serving-fail-closed-fixtures: status=ok "
           "serving_fail_closed_matrix=certified "
           "serving_paths=runtime-handoff,execution-artifact "
           "mismatch_cases=invalid-session,invalid-model-binding,invalid-owner,stale-ref,checksum-mismatch "
           "invalid_session=%" PRIu64 " invalid_model_binding=%" PRIu64
           " stale_ref=%" PRIu64 " checksum_mismatch=%" PRIu64
           " fail_closed=%" PRIu64 "\n",
           svc.metrics.invalid_session_count,
           svc.metrics.invalid_model_binding_count,
           svc.metrics.stale_ref_count,
           svc.metrics.checksum_mismatch_count,
           svc.metrics.fail_closed_count);
    return 0;
}

int mem_service_run_pretraining_fail_closed_fixture_check(void)
{
    static const char training_register[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "session_id=pt-session\n"
        "model_key=pt-model\n"
        "artifact_kind=training-step-commit\n"
        "artifact_id=global-step-42\n"
        "owner=3\n"
        "checksum=4242\n"
        "version=42\n"
        "idempotency_key=pretraining-step-42-v42\n";
    static const char training_match[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=pt-session\n"
        "expected_model_key=pt-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=3\n"
        "expected_version=42\n"
        "expected_checksum=4242\n";
    static const char training_bad_session[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=wrong-session\n"
        "expected_model_key=pt-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=3\n"
        "expected_version=42\n"
        "expected_checksum=4242\n";
    static const char training_bad_model[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=pt-session\n"
        "expected_model_key=wrong-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=3\n"
        "expected_version=42\n"
        "expected_checksum=4242\n";
    static const char training_bad_kind[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=pt-session\n"
        "expected_model_key=pt-model\n"
        "expected_artifact_kind=wrong-kind\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=3\n"
        "expected_version=42\n"
        "expected_checksum=4242\n";
    static const char training_bad_owner[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=pt-session\n"
        "expected_model_key=pt-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=99\n"
        "expected_version=42\n"
        "expected_checksum=4242\n";
    static const char training_bad_version[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=pt-session\n"
        "expected_model_key=pt-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=3\n"
        "expected_version=99\n"
        "expected_checksum=4242\n";
    static const char training_bad_checksum[] =
        "key=training/pretraining-matrix/global-step-42/commit\n"
        "expected_session_id=pt-session\n"
        "expected_model_key=pt-model\n"
        "expected_artifact_kind=training-step-commit\n"
        "expected_artifact_id=global-step-42\n"
        "expected_owner=3\n"
        "expected_version=42\n"
        "expected_checksum=4243\n";
    static struct mem_service svc;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status s;
    int failures = 0;

    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service pretraining-fail-closed-fixtures: init failed\n");
        return 1;
    }
#define PT_QUERY(op, q, expect)                                               \
    do {                                                                       \
        s = mem_service_handle_operation(&svc, (op), (q), response,            \
                                         sizeof(response), NULL, NULL);        \
        if (s != (expect)) {                                                   \
            fprintf(stderr,                                                    \
                    "mem_service pretraining-fail-closed-fixtures: " #op       \
                    " " #q " status=%u expected=%u\n",                         \
                    s, (expect));                                              \
            failures -= 1;                                                     \
        }                                                                      \
    } while (0)

    PT_QUERY(MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT, training_register,
             MEM_SERVICE_WIRE_STATUS_OK);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_match,
             MEM_SERVICE_WIRE_STATUS_OK);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_bad_session,
             MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_bad_model,
             MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_bad_kind,
             MEM_SERVICE_WIRE_STATUS_STALE_REF);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_bad_owner,
             MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_bad_version,
             MEM_SERVICE_WIRE_STATUS_STALE_REF);
    PT_QUERY(MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT, training_bad_checksum,
             MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH);
#undef PT_QUERY

    if (svc.metrics.invalid_session_count != 1U ||
        svc.metrics.invalid_model_binding_count != 2U ||
        svc.metrics.stale_ref_count != 2U ||
        svc.metrics.checksum_mismatch_count != 1U ||
        svc.metrics.fail_closed_count != 6U) {
        fprintf(stderr,
                "mem_service pretraining-fail-closed-fixtures: counter mismatch "
                "invalid_session=%" PRIu64 " invalid_model=%" PRIu64
                " stale_ref=%" PRIu64 " checksum=%" PRIu64 " fail_closed=%" PRIu64 "\n",
                svc.metrics.invalid_session_count,
                svc.metrics.invalid_model_binding_count,
                svc.metrics.stale_ref_count,
                svc.metrics.checksum_mismatch_count,
                svc.metrics.fail_closed_count);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service pretraining-fail-closed-fixtures: status=ok "
           "pretraining_fail_closed_matrix=certified "
           "pretraining_paths=training-step-commit "
           "mismatch_cases=invalid-session,invalid-model-binding,invalid-owner,stale-ref,checksum-mismatch "
           "invalid_session=%" PRIu64 " invalid_model_binding=%" PRIu64
           " stale_ref=%" PRIu64 " checksum_mismatch=%" PRIu64
           " fail_closed=%" PRIu64 "\n",
           svc.metrics.invalid_session_count,
           svc.metrics.invalid_model_binding_count,
           svc.metrics.stale_ref_count,
           svc.metrics.checksum_mismatch_count,
           svc.metrics.fail_closed_count);
    return 0;
}

/* Typed-binary payload data plane (additive to text-kv).
 *
 * Wire format (all multi-byte integers big-endian):
 *   magic "MSTP" (4 bytes) | version (u8) | field_count (u8) | fields...
 * Each field: type (u8) | name_len (u8) | name (name_len bytes) |
 *              value_len (u16) | value (value_len bytes)
 *   STRING (1): value is the raw bytes; U32 (2): value is 4 BE bytes;
 *   U64 (3): value is 8 BE bytes.
 *
 * Text-kv remains the default wire payload format; typed-binary is an opt-in
 * alternative representation. Decode rejects any version newer than
 * MAX_KNOWN_VERSION (forward-compat fail-closed). */
#define MEM_SERVICE_TYPED_PAYLOAD_MAGIC0 ((uint8_t)'M')
#define MEM_SERVICE_TYPED_PAYLOAD_MAGIC1 ((uint8_t)'S')
#define MEM_SERVICE_TYPED_PAYLOAD_MAGIC2 ((uint8_t)'T')
#define MEM_SERVICE_TYPED_PAYLOAD_MAGIC3 ((uint8_t)'P')
#define MEM_SERVICE_TYPED_PAYLOAD_VERSION 1U
#define MEM_SERVICE_TYPED_PAYLOAD_MAX_KNOWN_VERSION 1U
#define MEM_SERVICE_TYPED_PAYLOAD_MAX_FIELDS 32U
#define MEM_SERVICE_TYPED_PAYLOAD_MAX_NAME 32U
#define MEM_SERVICE_TYPED_PAYLOAD_MAX_VALUE 1024U

struct mem_service_typed_payload_field {
    uint8_t type;
    char name[MEM_SERVICE_TYPED_PAYLOAD_MAX_NAME];
    char string_value[MEM_SERVICE_TYPED_PAYLOAD_MAX_VALUE];
    uint64_t int_value;
};

static void mem_service_typed_put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t mem_service_typed_get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int mem_service_typed_payload_encode(
    const struct mem_service_typed_payload_field *fields,
    size_t field_count,
    uint8_t *buf,
    size_t buf_len)
{
    size_t offset = 0U;
    size_t i;

    if (fields == NULL || buf == NULL || field_count > 255U) {
        return -1;
    }
    if (buf_len < 6U) {
        return -1;
    }
    buf[0] = MEM_SERVICE_TYPED_PAYLOAD_MAGIC0;
    buf[1] = MEM_SERVICE_TYPED_PAYLOAD_MAGIC1;
    buf[2] = MEM_SERVICE_TYPED_PAYLOAD_MAGIC2;
    buf[3] = MEM_SERVICE_TYPED_PAYLOAD_MAGIC3;
    buf[4] = (uint8_t)MEM_SERVICE_TYPED_PAYLOAD_VERSION;
    buf[5] = (uint8_t)field_count;
    offset = 6U;
    for (i = 0U; i < field_count; ++i) {
        const struct mem_service_typed_payload_field *f = &fields[i];
        size_t name_len = strlen(f->name);
        size_t value_len = 0U;
        const uint8_t *value_bytes = NULL;
        uint8_t u32_be[4];
        uint8_t u64_be[8];
        size_t need;

        if (name_len == 0U || name_len > 255U) {
            return -1;
        }
        if (f->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING) {
            value_len = strlen(f->string_value);
            value_bytes = (const uint8_t *)f->string_value;
        } else if (f->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32) {
            uint32_t v = (uint32_t)f->int_value;
            u32_be[0] = (uint8_t)(v >> 24);
            u32_be[1] = (uint8_t)(v >> 16);
            u32_be[2] = (uint8_t)(v >> 8);
            u32_be[3] = (uint8_t)v;
            value_len = 4U;
            value_bytes = u32_be;
        } else if (f->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
            uint64_t v = f->int_value;
            int b;
            for (b = 0; b < 8; ++b) {
                u64_be[b] = (uint8_t)(v >> (56 - 8 * b));
            }
            value_len = 8U;
            value_bytes = u64_be;
        } else {
            return -1;
        }
        if (value_len > 65535U) {
            return -1;
        }
        need = 1U + 1U + name_len + 2U + value_len;
        if (offset + need > buf_len) {
            return -1;
        }
        buf[offset] = f->type;
        buf[offset + 1U] = (uint8_t)name_len;
        memcpy(buf + offset + 2U, f->name, name_len);
        mem_service_typed_put_u16_be(buf + offset + 2U + name_len,
                                     (uint16_t)value_len);
        memcpy(buf + offset + 4U + name_len, value_bytes, value_len);
        offset += need;
    }
    return (int)offset;
}

static int mem_service_typed_payload_decode(
    const uint8_t *buf,
    size_t buf_len,
    struct mem_service_typed_payload_field *out_fields,
    size_t max_fields,
    uint8_t *version_out)
{
    size_t offset = 0U;
    size_t field_count;
    size_t i;

    if (buf == NULL || out_fields == NULL) {
        return -1;
    }
    if (buf_len < 6U ||
        buf[0] != MEM_SERVICE_TYPED_PAYLOAD_MAGIC0 ||
        buf[1] != MEM_SERVICE_TYPED_PAYLOAD_MAGIC1 ||
        buf[2] != MEM_SERVICE_TYPED_PAYLOAD_MAGIC2 ||
        buf[3] != MEM_SERVICE_TYPED_PAYLOAD_MAGIC3) {
        return -1;
    }
    if (version_out != NULL) {
        *version_out = buf[4];
    }
    if (buf[4] > MEM_SERVICE_TYPED_PAYLOAD_MAX_KNOWN_VERSION) {
        return -1;
    }
    field_count = buf[5];
    if (field_count > max_fields) {
        return -1;
    }
    offset = 6U;
    for (i = 0U; i < field_count; ++i) {
        struct mem_service_typed_payload_field *f = &out_fields[i];
        uint8_t type;
        uint8_t name_len;
        uint16_t value_len;
        size_t need;

        if (offset + 2U > buf_len) {
            return -1;
        }
        type = buf[offset];
        name_len = buf[offset + 1U];
        need = 1U + 1U + (size_t)name_len + 2U;
        if (offset + need > buf_len) {
            return -1;
        }
        if (name_len == 0U || name_len >= MEM_SERVICE_TYPED_PAYLOAD_MAX_NAME) {
            return -1;
        }
        memset(f, 0, sizeof(*f));
        f->type = type;
        memcpy(f->name, buf + offset + 2U, name_len);
        f->name[name_len] = '\0';
        value_len = mem_service_typed_get_u16_be(buf + offset + 2U + name_len);
        if (value_len > MEM_SERVICE_TYPED_PAYLOAD_MAX_VALUE ||
            offset + need + value_len > buf_len) {
            return -1;
        }
        if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING) {
            if (value_len >= MEM_SERVICE_TYPED_PAYLOAD_MAX_VALUE) {
                return -1;
            }
            memcpy(f->string_value, buf + offset + 4U + name_len, value_len);
            f->string_value[value_len] = '\0';
        } else if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32) {
            const uint8_t *p = buf + offset + 4U + name_len;
            if (value_len != 4U) {
                return -1;
            }
            f->int_value = (uint64_t)(((uint32_t)p[0] << 24) |
                                      ((uint32_t)p[1] << 16) |
                                      ((uint32_t)p[2] << 8) |
                                      (uint32_t)p[3]);
        } else if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
            const uint8_t *p = buf + offset + 4U + name_len;
            uint64_t v = 0U;
            int b;
            if (value_len != 8U) {
                return -1;
            }
            for (b = 0; b < 8; ++b) {
                v = (v << 8) | (uint64_t)p[b];
            }
            f->int_value = v;
        } else {
            return -1;
        }
        offset += need + value_len;
    }
    return (int)field_count;
}

int mem_service_run_typed_payload_fixture_check(void)
{
    static const char string_expect[] = "typed-payload-string-value";
    struct mem_service_typed_payload_field encode_fields[3];
    struct mem_service_typed_payload_field decode_fields[MEM_SERVICE_TYPED_PAYLOAD_MAX_FIELDS];
    uint8_t buf[512];
    uint8_t future_buf[512];
    int encoded;
    int decoded;
    uint8_t version = 0U;
    int failures = 0;

    memset(encode_fields, 0, sizeof(encode_fields));
    encode_fields[0].type = MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING;
    snprintf(encode_fields[0].name, sizeof(encode_fields[0].name), "session_id");
    snprintf(encode_fields[0].string_value, sizeof(encode_fields[0].string_value),
             "%s", string_expect);
    encode_fields[1].type = MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32;
    snprintf(encode_fields[1].name, sizeof(encode_fields[1].name), "owner_node");
    encode_fields[1].int_value = 2147483647U; /* large u32, exercises all 4 bytes */
    encode_fields[2].type = MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64;
    snprintf(encode_fields[2].name, sizeof(encode_fields[2].name), "checksum");
    encode_fields[2].int_value = 18364758544493064720ULL; /* > 2^63, all 8 bytes */

    encoded = mem_service_typed_payload_encode(encode_fields, 3U, buf, sizeof(buf));
    if (encoded <= 0) {
        fprintf(stderr,
                "mem_service typed-payload-fixtures: encode failed rc=%d\n",
                encoded);
        return 1;
    }
    decoded = mem_service_typed_payload_decode(buf, (size_t)encoded, decode_fields,
                                               MEM_SERVICE_TYPED_PAYLOAD_MAX_FIELDS,
                                               &version);
    if (decoded != 3 || version != MEM_SERVICE_TYPED_PAYLOAD_VERSION) {
        fprintf(stderr,
                "mem_service typed-payload-fixtures: decode header mismatch "
                "decoded=%d version=%u\n",
                decoded,
                version);
        failures -= 1;
    }
    if (decoded == 3) {
        if (decode_fields[0].type != MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING ||
            strcmp(decode_fields[0].name, "session_id") != 0 ||
            strcmp(decode_fields[0].string_value, string_expect) != 0 ||
            decode_fields[1].type != MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32 ||
            strcmp(decode_fields[1].name, "owner_node") != 0 ||
            decode_fields[1].int_value != 2147483647U ||
            decode_fields[2].type != MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64 ||
            strcmp(decode_fields[2].name, "checksum") != 0 ||
            decode_fields[2].int_value != 18364758544493064720ULL) {
            fprintf(stderr,
                    "mem_service typed-payload-fixtures: round-trip field mismatch\n");
            failures -= 1;
        }
    }
    /* Forward-compat version gate: a future-version buffer must be rejected. */
    memcpy(future_buf, buf, (size_t)encoded);
    future_buf[4] = (uint8_t)(MEM_SERVICE_TYPED_PAYLOAD_MAX_KNOWN_VERSION + 1U);
    if (mem_service_typed_payload_decode(future_buf, (size_t)encoded, decode_fields,
                                         MEM_SERVICE_TYPED_PAYLOAD_MAX_FIELDS,
                                         NULL) != -1) {
        fprintf(stderr,
                "mem_service typed-payload-fixtures: future version not rejected\n");
        failures -= 1;
    }
    /* Malformed input must fail-closed, not crash: truncated header, bad magic,
     * and a value_len that overruns the buffer. */
    if (mem_service_typed_payload_decode(buf, 3U, decode_fields,
                                         MEM_SERVICE_TYPED_PAYLOAD_MAX_FIELDS,
                                         NULL) != -1 ||
        mem_service_typed_payload_decode((const uint8_t *)"XSTP\x01\x00", 6U,
                                         decode_fields,
                                         MEM_SERVICE_TYPED_PAYLOAD_MAX_FIELDS,
                                         NULL) != -1) {
        fprintf(stderr,
                "mem_service typed-payload-fixtures: malformed input not rejected\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service typed-payload-fixtures: status=ok "
           "wire_payload_typed_binary_format=typed-binary-v1 "
           "wire_payload_text_kv_format=text-kv "
           "round_trip_fields=3 "
           "version=%u version_gate=reject-unknown-future "
           "malformed_input=fail-closed encoded_bytes=%d\n",
           MEM_SERVICE_TYPED_PAYLOAD_VERSION,
           encoded);
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
    char future_root[160];
    char future_catalog_dir[192];
    char future_manifest_path[224];

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
    snprintf(future_root,
             sizeof(future_root),
             "/tmp/linqu_mem_service_catalog_future_%ld",
             (long)getpid());
    if (mem_service_join_path(future_catalog_dir,
                              sizeof(future_catalog_dir),
                              future_root,
                              "catalog") != 0 ||
        mem_service_make_catalog_path(future_root,
                                      MEM_SERVICE_DURABLE_CATALOG_MANIFEST,
                                      future_manifest_path,
                                      sizeof(future_manifest_path)) != 0) {
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
                                   "catalog_schema_version=1\n") ||
        !mem_service_file_contains(manifest_path,
                                   "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1") ||
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
    /* Migration policy: current schema version (1) must be accepted, a legacy
     * manifest without a schema version must be upgraded to v1, and an unknown
     * future version must be refused. */
    if (mem_service_admit_or_migrate_catalog_schema_version(storage_root,
                                                            store_path) != 0) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: current schema version "
                "rejected\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        return 1;
    }
    {
        FILE *legacy_file = fopen(manifest_path, "w");

        if (legacy_file == NULL ||
            fprintf(legacy_file,
                    "%s\nlayout=storage-root-v1\nstore_path=%s\n",
                    MEM_SERVICE_DURABLE_CATALOG_MAGIC,
                    store_path) < 0 ||
            fclose(legacy_file) != 0) {
            if (legacy_file != NULL) {
                fclose(legacy_file);
            }
            fprintf(stderr,
                    "mem_service durable-catalog-fixtures: legacy manifest write "
                    "failed\n");
            unlink(manifest_path);
            rmdir(quarantine_dir);
            rmdir(block_dir);
            rmdir(catalog_dir);
            rmdir(storage_root);
            return 1;
        }
    }
    if (mem_service_admit_or_migrate_catalog_schema_version(storage_root,
                                                            store_path) != 0 ||
        !mem_service_file_contains(manifest_path,
                                   "catalog_schema_version=1\n") ||
        !mem_service_file_contains(manifest_path,
                                   "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1")) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: legacy schema migration "
                "failed\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        return 1;
    }
    if (mem_service_ensure_dir(future_root) != 0 ||
        mem_service_ensure_dir(future_catalog_dir) != 0) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: future root setup failed\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        return 1;
    }
    {
        FILE *future_file = fopen(future_manifest_path, "w");

        if (future_file == NULL ||
            fprintf(future_file,
                    "%s\nlayout=storage-root-v1\ncatalog_schema_version=99\n",
                    MEM_SERVICE_DURABLE_CATALOG_MAGIC) < 0 ||
            fclose(future_file) != 0) {
            if (future_file != NULL) {
                fclose(future_file);
            }
            fprintf(stderr,
                    "mem_service durable-catalog-fixtures: future manifest write "
                    "failed\n");
            unlink(manifest_path);
            rmdir(quarantine_dir);
            rmdir(block_dir);
            rmdir(catalog_dir);
            rmdir(storage_root);
            unlink(future_manifest_path);
            rmdir(future_catalog_dir);
            rmdir(future_root);
            return 1;
        }
    }
    if (mem_service_admit_or_migrate_catalog_schema_version(future_root,
                                                            store_path) !=
        -1) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: unknown future schema "
                "version not refused\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        unlink(future_manifest_path);
        rmdir(future_catalog_dir);
        rmdir(future_root);
        return 1;
    }
    {
        FILE *malformed_file = fopen(future_manifest_path, "w");

        if (malformed_file == NULL ||
            fprintf(malformed_file,
                    "%s\nlayout=storage-root-v1\ncatalog_schema_version=1abc\n",
                    MEM_SERVICE_DURABLE_CATALOG_MAGIC) < 0 ||
            fclose(malformed_file) != 0) {
            if (malformed_file != NULL) {
                fclose(malformed_file);
            }
            fprintf(stderr,
                    "mem_service durable-catalog-fixtures: malformed manifest "
                    "write failed\n");
            unlink(manifest_path);
            rmdir(quarantine_dir);
            rmdir(block_dir);
            rmdir(catalog_dir);
            rmdir(storage_root);
            unlink(future_manifest_path);
            rmdir(future_catalog_dir);
            rmdir(future_root);
            return 1;
        }
    }
    if (mem_service_admit_or_migrate_catalog_schema_version(future_root,
                                                            store_path) !=
        -1) {
        fprintf(stderr,
                "mem_service durable-catalog-fixtures: malformed schema version "
                "not refused\n");
        unlink(manifest_path);
        rmdir(quarantine_dir);
        rmdir(block_dir);
        rmdir(catalog_dir);
        rmdir(storage_root);
        unlink(future_manifest_path);
        rmdir(future_catalog_dir);
        rmdir(future_root);
        return 1;
    }
    unlink(manifest_path);
    rmdir(quarantine_dir);
    rmdir(block_dir);
    rmdir(catalog_dir);
    rmdir(storage_root);
    unlink(future_manifest_path);
    rmdir(future_catalog_dir);
    rmdir(future_root);
    printf("mem_service durable-catalog-fixtures: status=ok layout=storage-root-v1 "
           "catalog_schema_version=%d migration_policy=migrate-legacy-to-v1-reject-future "
           "manifest=%s store=%s payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1\n",
           MEM_SERVICE_DURABLE_CATALOG_SCHEMA_VERSION,
           MEM_SERVICE_DURABLE_CATALOG_MANIFEST,
           "catalog/store.snapshot");
    return 0;
}

int mem_service_run_chunked_block_fixture_check(void)
{
    char storage_root[160];
    char blocks_dir[192];
    char payload_path[192];
    char dir_path[224];
    char manifest_path[240];
    char chunk_path[240];
    uint8_t payload_bytes[2500];
    uint64_t expected_checksum;
    char payload[160];
    struct mem_service_record record;
    enum mem_service_wire_status status;
    FILE *file;
    size_t i;

    snprintf(storage_root,
             sizeof(storage_root),
             "/tmp/linqu_mem_service_chunked_block_%ld",
             (long)getpid());
    if (mem_service_join_path(blocks_dir,
                              sizeof(blocks_dir),
                              storage_root,
                              "blocks") != 0 ||
        snprintf(payload_path,
                 sizeof(payload_path),
                 "%s/chunked-source.%ld",
                 storage_root,
                 (long)getpid()) >= (int)sizeof(payload_path)) {
        fprintf(stderr, "mem_service chunked-block-fixtures: path setup failed\n");
        return 1;
    }
    unlink(payload_path);
    rmdir(blocks_dir);
    rmdir(storage_root);
    if (mem_service_ensure_dir(storage_root) != 0 ||
        mem_service_ensure_dir(blocks_dir) != 0) {
        fprintf(stderr, "mem_service chunked-block-fixtures: storage setup failed\n");
        return 1;
    }
    for (i = 0U; i < sizeof(payload_bytes); ++i) {
        payload_bytes[i] = (uint8_t)((i * 7U) + 3U);
    }
    expected_checksum = mem_service_checksum_bytes(payload_bytes,
                                                   sizeof(payload_bytes));
    file = fopen(payload_path, "wb");
    if (file == NULL ||
        fwrite(payload_bytes, 1U, sizeof(payload_bytes), file) !=
            sizeof(payload_bytes) ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr, "mem_service chunked-block-fixtures: source write failed\n");
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (snprintf(payload,
                 sizeof(payload),
                 "payload_kind=%u\nchecksum=%" PRIu64 "\nbacking_len=%zu\n",
                 MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK,
                 expected_checksum,
                 sizeof(payload_bytes)) >= (int)sizeof(payload)) {
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    memset(&record, 0, sizeof(record));
    status = mem_service_write_payload_block(storage_root,
                                             payload,
                                             NULL,
                                             payload_path,
                                             &record);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        record.object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK ||
        record.object_payload_checksum != expected_checksum ||
        record.object_backing_len != sizeof(payload_bytes)) {
        fprintf(stderr,
                "mem_service chunked-block-fixtures: write mismatch status=%u kind=%u\n",
                status,
                record.object_payload_kind);
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (mem_service_make_chunked_block_dir_path(storage_root,
                                                expected_checksum,
                                                dir_path,
                                                sizeof(dir_path)) != 0 ||
        !mem_service_path_is_dir(dir_path) ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_CHUNKED_BLOCK_MANIFEST) != 0 ||
        !mem_service_file_contains(manifest_path, "chunk_count=3\n") ||
        !mem_service_file_contains(manifest_path, "chunk_size=1024\n") ||
        mem_service_make_chunked_block_chunk_path(dir_path,
                                                  0U,
                                                  chunk_path,
                                                  sizeof(chunk_path)) != 0 ||
        !mem_service_file_contains(chunk_path, "")) {
        fprintf(stderr,
                "mem_service chunked-block-fixtures: chunked layout mismatch\n");
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service chunked-block-fixtures: validate mismatch status=%u\n",
                status);
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    /* Corrupt one byte inside chunk 1's read window and re-validate: the
     * reassembled checksum must no longer match and the block must be
     * quarantined (fail-closed). */
    if (mem_service_make_chunked_block_chunk_path(dir_path,
                                                  1U,
                                                  chunk_path,
                                                  sizeof(chunk_path)) != 0) {
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    file = fopen(chunk_path, "r+b");
    if (file == NULL ||
        fseek(file, 5, SEEK_SET) != 0 ||
        fputc('X', file) == EOF ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr,
                "mem_service chunked-block-fixtures: corruption setup failed\n");
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status != MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH ||
        mem_service_path_is_dir(dir_path)) {
        fprintf(stderr,
                "mem_service chunked-block-fixtures: corruption not quarantined "
                "status=%u\n",
                status);
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    unlink(payload_path);
    rmdir(blocks_dir);
    rmdir(storage_root);
    printf("mem_service chunked-block-fixtures: status=ok "
           "payload_block_backend=sealed-chunked-block-v1 "
           "chunk_size=%u chunks=3 total_len=%zu "
           "integrity=fail-closed-quarantine\n",
           MEM_SERVICE_CHUNKED_BLOCK_SIZE,
           sizeof(payload_bytes));
    return 0;
}

int mem_service_run_transport_block_fixture_check(void)
{
    char storage_root[160];
    char blocks_dir[240];
    char remote_blocks_dir[240];
    char payload_path[240];
    char dir_path[240];
    char manifest_path[240];
    char block_path[240];
    uint8_t payload_bytes[1537];
    uint64_t expected_checksum;
    char payload[160];
    struct mem_service_record record;
    enum mem_service_wire_status status;
    FILE *file;
    size_t i;

    snprintf(storage_root,
             sizeof(storage_root),
             "/tmp/linqu_mem_service_transport_block_%ld",
             (long)getpid());
    if (mem_service_join_path(blocks_dir,
                              sizeof(blocks_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(remote_blocks_dir,
                              sizeof(remote_blocks_dir),
                              storage_root,
                              "remote-blocks") != 0 ||
        snprintf(payload_path,
                 sizeof(payload_path),
                 "%s/transport-source.%ld",
                 storage_root,
                 (long)getpid()) >= (int)sizeof(payload_path)) {
        fprintf(stderr, "mem_service transport-block-fixtures: path setup failed\n");
        return 1;
    }
    unlink(payload_path);
    rmdir(blocks_dir);
    rmdir(remote_blocks_dir);
    rmdir(storage_root);
    if (mem_service_ensure_dir(storage_root) != 0 ||
        mem_service_ensure_dir(blocks_dir) != 0 ||
        mem_service_ensure_dir(remote_blocks_dir) != 0) {
        fprintf(stderr, "mem_service transport-block-fixtures: storage setup failed\n");
        return 1;
    }
    for (i = 0U; i < sizeof(payload_bytes); ++i) {
        payload_bytes[i] = (uint8_t)((i * 11U) + 19U);
    }
    expected_checksum = mem_service_checksum_bytes(payload_bytes,
                                                   sizeof(payload_bytes));
    file = fopen(payload_path, "wb");
    if (file == NULL ||
        fwrite(payload_bytes, 1U, sizeof(payload_bytes), file) !=
            sizeof(payload_bytes) ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr, "mem_service transport-block-fixtures: source write failed\n");
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (snprintf(payload,
                 sizeof(payload),
                 "payload_kind=%u\nchecksum=%" PRIu64 "\nbacking_len=%zu\n",
                 MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK,
                 expected_checksum,
                 sizeof(payload_bytes)) >= (int)sizeof(payload)) {
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    memset(&record, 0, sizeof(record));
    status = mem_service_write_payload_block(storage_root,
                                             payload,
                                             NULL,
                                             payload_path,
                                             &record);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        record.object_payload_kind !=
            MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK ||
        record.object_payload_checksum != expected_checksum ||
        record.object_backing_len != sizeof(payload_bytes)) {
        fprintf(stderr,
                "mem_service transport-block-fixtures: write mismatch status=%u "
                "kind=%u\n",
                status,
                record.object_payload_kind);
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (mem_service_make_transport_block_dir_path(storage_root,
                                                  expected_checksum,
                                                  dir_path,
                                                  sizeof(dir_path)) != 0 ||
        !mem_service_path_is_dir(dir_path) ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0 ||
        !mem_service_file_contains(manifest_path,
                                   "backend=transport-loopback-block-v1\n") ||
        !mem_service_file_contains(manifest_path, "transport=file-copy-v1\n") ||
        !mem_service_file_contains(block_path, "")) {
        fprintf(stderr,
                "mem_service transport-block-fixtures: layout mismatch\n");
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service transport-block-fixtures: validate mismatch status=%u\n",
                status);
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    file = fopen(block_path, "r+b");
    if (file == NULL ||
        fseek(file, 17, SEEK_SET) != 0 ||
        fputc('Z', file) == EOF ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr,
                "mem_service transport-block-fixtures: corruption setup failed\n");
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status != MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH ||
        mem_service_path_is_dir(dir_path)) {
        fprintf(stderr,
                "mem_service transport-block-fixtures: corruption not quarantined "
                "status=%u\n",
                status);
        unlink(payload_path);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    unlink(payload_path);
    rmdir(blocks_dir);
    rmdir(remote_blocks_dir);
    rmdir(storage_root);
    printf("mem_service transport-block-fixtures: status=ok "
           "payload_block_backend=transport-loopback-block-v1 "
           "transport=file-copy-v1 total_len=%zu "
           "integrity=fail-closed-quarantine "
           "network_transport=not-certified\n",
           sizeof(payload_bytes));
    return 0;
}

static int mem_service_fixture_write_all(int fd, const uint8_t *bytes, size_t len)
{
    size_t done = 0U;

    while (done < len) {
        ssize_t rc = write(fd, bytes + done, len - done);

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

static int mem_service_start_tcp_payload_fixture_source(const uint8_t *payload,
                                                        size_t payload_len,
                                                        uint16_t *port_out,
                                                        pid_t *child_out)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int server_fd;
    pid_t child;
    int reuse = 1;

    if (payload == NULL || payload_len == 0U || port_out == NULL ||
        child_out == NULL) {
        return -1;
    }
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return -1;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        bind(server_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server_fd, 1) != 0 ||
        getsockname(server_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        close(server_fd);
        return -1;
    }
    child = fork();
    if (child < 0) {
        close(server_fd);
        return -1;
    }
    if (child == 0) {
        int client_fd = accept(server_fd, NULL, NULL);
        int rc = 1;

        if (client_fd >= 0) {
            rc = mem_service_fixture_write_all(client_fd, payload, payload_len) == 0
                     ? 0
                     : 1;
            close(client_fd);
        }
        close(server_fd);
        _exit(rc);
    }
    close(server_fd);
    *port_out = ntohs(addr.sin_port);
    *child_out = child;
    return 0;
}

int mem_service_run_tcp_payload_fixture_source(const char *listen_spec,
                                               uint64_t payload_len)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char bound_ip[INET_ADDRSTRLEN];
    uint8_t *payload;
    uint64_t checksum;
    uint64_t i;
    int server_fd;
    int client_fd;
    int reuse = 1;
    int rc = 1;

    if (listen_spec == NULL || listen_spec[0] == '\0' ||
        payload_len == 0U || payload_len > (64ULL * 1024ULL * 1024ULL)) {
        fprintf(stderr,
                "mem_service remote-transport-serve-fixture: invalid listen or payload_len\n");
        return 2;
    }
    payload = (uint8_t *)malloc((size_t)payload_len);
    if (payload == NULL) {
        fprintf(stderr,
                "mem_service remote-transport-serve-fixture: payload allocation failed\n");
        return 1;
    }
    for (i = 0U; i < payload_len; ++i) {
        payload[i] = (uint8_t)((i * 13U) + 23U);
    }
    checksum = mem_service_checksum_bytes(payload, payload_len);
    if (mem_service_parse_tcp_payload_source(listen_spec, &addr) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-serve-fixture: invalid listen spec\n");
        free(payload);
        return 2;
    }
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("mem_service remote-transport-serve-fixture: socket");
        free(payload);
        return 1;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(server_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server_fd, 1) != 0 ||
        getsockname(server_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        perror("mem_service remote-transport-serve-fixture: listen");
        close(server_fd);
        free(payload);
        return 1;
    }
    if (inet_ntop(AF_INET, &addr.sin_addr, bound_ip, sizeof(bound_ip)) == NULL) {
        snprintf(bound_ip, sizeof(bound_ip), "0.0.0.0");
    }
    printf("mem_service remote-transport-serve-fixture: status=ready "
           "listen=tcp:%s:%u payload_len=%" PRIu64 " payload_checksum=0x%016" PRIx64 "\n",
           bound_ip,
           (unsigned)ntohs(addr.sin_port),
           payload_len,
           checksum);
    fflush(stdout);
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd >= 0) {
        rc = mem_service_fixture_write_all(client_fd, payload, (size_t)payload_len) == 0
                 ? 0
                 : 1;
        close(client_fd);
    }
    close(server_fd);
    free(payload);
    if (rc != 0) {
        fprintf(stderr,
                "mem_service remote-transport-serve-fixture: payload write failed\n");
        return 1;
    }
    printf("mem_service remote-transport-serve-fixture: status=done "
           "payload_len=%" PRIu64 " payload_checksum=0x%016" PRIx64 "\n",
           payload_len,
           checksum);
    return 0;
}

int mem_service_run_network_transport_block_fixture_check(void)
{
    char storage_root[160];
    char blocks_dir[240];
    char remote_blocks_dir[240];
    char payload_source[80];
    char dir_path[240];
    char manifest_path[240];
    char block_path[240];
    uint8_t payload_bytes[2049];
    uint64_t expected_checksum;
    char payload[160];
    struct mem_service_record record;
    enum mem_service_wire_status status;
    uint16_t port = 0;
    pid_t child = -1;
    int child_status = 0;
    FILE *file;
    size_t i;

    snprintf(storage_root,
             sizeof(storage_root),
             "/tmp/linqu_mem_service_tcp_block_%ld",
             (long)getpid());
    if (mem_service_join_path(blocks_dir,
                              sizeof(blocks_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(remote_blocks_dir,
                              sizeof(remote_blocks_dir),
                              storage_root,
                              "remote-blocks") != 0) {
        fprintf(stderr, "mem_service network-transport-block-fixtures: path setup failed\n");
        return 1;
    }
    rmdir(blocks_dir);
    rmdir(remote_blocks_dir);
    rmdir(storage_root);
    if (mem_service_ensure_dir(storage_root) != 0 ||
        mem_service_ensure_dir(blocks_dir) != 0 ||
        mem_service_ensure_dir(remote_blocks_dir) != 0) {
        fprintf(stderr, "mem_service network-transport-block-fixtures: storage setup failed\n");
        return 1;
    }
    for (i = 0U; i < sizeof(payload_bytes); ++i) {
        payload_bytes[i] = (uint8_t)((i * 13U) + 23U);
    }
    expected_checksum = mem_service_checksum_bytes(payload_bytes,
                                                   sizeof(payload_bytes));
    if (mem_service_start_tcp_payload_fixture_source(payload_bytes,
                                                     sizeof(payload_bytes),
                                                     &port,
                                                     &child) != 0 ||
        snprintf(payload_source,
                 sizeof(payload_source),
                 "tcp:127.0.0.1:%u",
                 (unsigned)port) >= (int)sizeof(payload_source)) {
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: tcp source setup failed\n");
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (snprintf(payload,
                 sizeof(payload),
                 "payload_kind=%u\nchecksum=%" PRIu64 "\nbacking_len=%zu\n",
                 MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK,
                 expected_checksum,
                 sizeof(payload_bytes)) >= (int)sizeof(payload)) {
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    memset(&record, 0, sizeof(record));
    status = mem_service_write_payload_block(storage_root,
                                             payload,
                                             NULL,
                                             payload_source,
                                             &record);
    if (waitpid(child, &child_status, 0) < 0 ||
        !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: tcp source failed\n");
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        record.object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK ||
        record.object_payload_checksum != expected_checksum ||
        record.object_backing_len != sizeof(payload_bytes)) {
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: write mismatch "
                "status=%u kind=%u\n",
                status,
                record.object_payload_kind);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    if (mem_service_make_transport_tcp_block_dir_path(storage_root,
                                                      expected_checksum,
                                                      dir_path,
                                                      sizeof(dir_path)) != 0 ||
        !mem_service_path_is_dir(dir_path) ||
        mem_service_join_path(manifest_path,
                              sizeof(manifest_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_MANIFEST) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0 ||
        !mem_service_file_contains(manifest_path,
                                   "backend=transport-tcp-block-v1\n") ||
        !mem_service_file_contains(manifest_path, "transport=tcp-loopback-v1\n") ||
        !mem_service_file_contains(block_path, "")) {
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: layout mismatch\n");
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: validate mismatch "
                "status=%u\n",
                status);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    file = fopen(block_path, "r+b");
    if (file == NULL ||
        fseek(file, 31, SEEK_SET) != 0 ||
        fputc('N', file) == EOF ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: corruption setup failed\n");
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status != MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH ||
        mem_service_path_is_dir(dir_path)) {
        fprintf(stderr,
                "mem_service network-transport-block-fixtures: corruption not "
                "quarantined status=%u\n",
                status);
        rmdir(blocks_dir);
        rmdir(remote_blocks_dir);
        rmdir(storage_root);
        return 1;
    }
    rmdir(blocks_dir);
    rmdir(remote_blocks_dir);
    rmdir(storage_root);
    printf("mem_service network-transport-block-fixtures: status=ok "
           "payload_block_backend=transport-tcp-block-v1 "
           "transport=tcp-loopback-v1 total_len=%zu "
           "integrity=fail-closed-quarantine "
           "network_transport=tcp-loopback-certified\n",
           sizeof(payload_bytes));
    return 0;
}

int mem_service_probe_transport_tcp_payload_block(
    const char *storage_root,
    const char *payload_source,
    struct mem_service_remote_transport_probe_result *result)
{
    char blocks_dir[512];
    char remote_blocks_dir[512];
    char dir_path[512];
    char block_path[512];
    char payload[96];
    struct mem_service_record record;
    enum mem_service_wire_status status;
    FILE *file;

    if (result == NULL) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (storage_root == NULL || storage_root[0] == '\0' ||
        payload_source == NULL || payload_source[0] == '\0') {
        return -1;
    }
    if (mem_service_join_path(blocks_dir,
                              sizeof(blocks_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(remote_blocks_dir,
                              sizeof(remote_blocks_dir),
                              storage_root,
                              "remote-blocks") != 0 ||
        mem_service_ensure_dir(storage_root) != 0 ||
        mem_service_ensure_dir(blocks_dir) != 0 ||
        mem_service_ensure_dir(remote_blocks_dir) != 0) {
        return -1;
    }
    if (snprintf(payload,
                 sizeof(payload),
                 "payload_kind=%u\n",
                 MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK) >=
        (int)sizeof(payload)) {
        return -1;
    }
    memset(&record, 0, sizeof(record));
    status = mem_service_write_payload_block(storage_root,
                                             payload,
                                             NULL,
                                             payload_source,
                                             &record);
    if (status != MEM_SERVICE_WIRE_STATUS_OK ||
        record.object_payload_kind != MEM_SERVICE_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK ||
        record.object_backing_len == 0U ||
        record.object_payload_checksum == 0U) {
        return 0;
    }
    result->payload_block_round_trip = true;
    result->payload_len = record.object_backing_len;
    result->payload_checksum = record.object_payload_checksum;
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status == MEM_SERVICE_WIRE_STATUS_OK) {
        result->payload_checksum_validation = true;
    } else {
        return 0;
    }
    if (mem_service_make_transport_tcp_block_dir_path(storage_root,
                                                      record.object_payload_checksum,
                                                      dir_path,
                                                      sizeof(dir_path)) != 0 ||
        mem_service_join_path(block_path,
                              sizeof(block_path),
                              dir_path,
                              MEM_SERVICE_TRANSPORT_BLOCK_PAYLOAD) != 0) {
        return 0;
    }
    file = fopen(block_path, "r+b");
    if (file == NULL ||
        fseek(file, 0, SEEK_SET) != 0 ||
        fputc('X', file) == EOF ||
        fclose(file) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    status = mem_service_validate_payload_block(storage_root, &record);
    if (status == MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH &&
        !mem_service_path_is_dir(dir_path)) {
        result->payload_corruption_fail_closed = true;
    }
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

static bool mem_service_payload_get_u32_checked(const char *payload,
                                                const char *name,
                                                uint32_t *out)
{
    uint64_t parsed;

    if (!mem_service_payload_get_u64_checked(payload, name, &parsed) ||
        parsed > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
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

static const char *mem_service_object_backend_name(uint32_t backend_kind)
{
    if (backend_kind == MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA) {
        return "ub-ssd-gsva-v1";
    }
    return "legacy-payload";
}

static bool mem_service_payload_selects_ub_ssd_backend(const char *payload)
{
    char backend[64];

    if (mem_service_payload_get_u32(payload, "backend_kind", 0) ==
        MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA) {
        return true;
    }
    if (!mem_service_payload_get_string(payload, "backend", backend, sizeof(backend))) {
        return false;
    }
    return strcmp(backend, "ub-ssd-gsva-v1") == 0 ||
           strcmp(backend, "ub_ssd_gsva_v1") == 0;
}

static bool mem_service_payload_ub_ssd_write_requested(const char *payload)
{
    return mem_service_payload_get_u32(payload, "backend_write", 0) != 0U;
}

static bool mem_service_payload_ub_ssd_read_requested(const char *payload)
{
    return mem_service_payload_get_u32(payload, "backend_read", 0) != 0U;
}

#ifdef __linux__
static enum mem_service_wire_status mem_service_ub_ssd_status_to_wire(
    int32_t status)
{
    if (status == MEM_SERVICE_UB_SSD_OK) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_STALE_EPOCH ||
        status == MEM_SERVICE_UB_SSD_ERR_SEGMENT_RETIRED) {
        return MEM_SERVICE_WIRE_STATUS_STALE_REF;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_COH_TIMEOUT) {
        return MEM_SERVICE_WIRE_STATUS_TIMEOUT;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_CHECKSUM) {
        return MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH;
    }
    if (status == MEM_SERVICE_UB_SSD_ERR_VERSION_CONFLICT) {
        return MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
    }
    return MEM_SERVICE_WIRE_STATUS_INTERNAL;
}

static bool mem_service_payload_get_ub_ssd_buffer_desc(
    const char *payload,
    struct mem_service_ub_ssd_buffer_desc_v1 *desc)
{
    uint64_t gsva_base = 0;
    uint64_t bytes = 0;
    uint64_t segment_id = 0;
    uint64_t home_va = 0;
    uint64_t size = 0;
    uint64_t epoch = 0;
    uint64_t parsed = 0;
    uint32_t token_id = 0;
    uint32_t token_value = 0;

    if (payload == NULL || desc == NULL) {
        return false;
    }
    memset(desc, 0, sizeof(*desc));
    if (!mem_service_payload_get_u64_checked(payload,
                                             "backend_buffer_gsva_base",
                                             &gsva_base) ||
        !mem_service_payload_get_u64_checked(payload,
                                             "backend_buffer_bytes",
                                             &bytes) ||
        !mem_service_payload_get_u64_checked(payload,
                                             "backend_buffer_key_segment_id",
                                             &segment_id) ||
        !mem_service_payload_get_u64_checked(payload,
                                             "backend_buffer_key_home_va",
                                             &home_va) ||
        !mem_service_payload_get_u64_checked(payload,
                                             "backend_buffer_key_size",
                                             &size) ||
        !mem_service_payload_get_u64_checked(payload,
                                             "backend_buffer_key_epoch",
                                             &epoch) ||
        !mem_service_payload_get_u32_checked(payload,
                                             "backend_buffer_token_id",
                                             &token_id) ||
        !mem_service_payload_get_u32_checked(payload,
                                             "backend_buffer_token_value",
                                             &token_value) ||
        gsva_base == 0U || bytes == 0U || segment_id == 0U ||
        home_va == 0U || size == 0U || epoch == 0U ||
        token_id == 0U || token_value == 0U) {
        return false;
    }
    desc->gsva_base = gsva_base;
    desc->bytes = bytes;
    desc->key.segment_id = segment_id;
    desc->key.home_va = home_va;
    desc->key.size = size;
    desc->key.epoch = epoch;
    desc->token_id = token_id;
    desc->token_value = token_value;
    desc->key.version = mem_service_payload_get_u32(payload,
                                                    "backend_buffer_key_version",
                                                    1U);
    desc->key.flags = mem_service_payload_get_u32(payload,
                                                  "backend_buffer_key_flags",
                                                  0U);
    desc->key.p_tag = mem_service_payload_get_u32(payload,
                                                  "backend_buffer_key_p_tag",
                                                  0U);
    desc->key.cache_policy =
        mem_service_payload_get_u32(payload,
                                    "backend_buffer_key_cache_policy",
                                    0U);
    desc->key.vmid = mem_service_payload_get_u64(payload,
                                                 "backend_buffer_key_vmid",
                                                 0U);
    desc->key.asid = mem_service_payload_get_u64(payload,
                                                 "backend_buffer_key_asid",
                                                 0U);
    desc->key.pte_offset =
        mem_service_payload_get_u64(payload,
                                    "backend_buffer_key_pte_offset",
                                    0U);
    if (mem_service_payload_get_u64_checked(payload,
                                            "backend_buffer_key_p_tag",
                                            &parsed) &&
        parsed > UINT32_MAX) {
        return false;
    }
    if (mem_service_payload_get_u64_checked(payload,
                                            "backend_buffer_key_cache_policy",
                                            &parsed) &&
        parsed > UINT32_MAX) {
        return false;
    }
    return desc->key.version == 1U;
}
#endif

static enum mem_service_wire_status mem_service_ub_ssd_submit(
    const char *payload,
    uint32_t opcode,
    const struct mem_service_record *record,
    struct mem_service_ub_ssd_cpl_v1 *cpl)
{
#ifndef __linux__
    (void)payload;
    (void)opcode;
    (void)record;
    (void)cpl;
    return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
#else
    struct mem_service_ub_ssd_cmd_v1 cmd;
    char device_path[128];
    int fd;
    int rc;

    if (payload == NULL || record == NULL || cpl == NULL) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    memset(&cmd, 0, sizeof(cmd));
    memset(cpl, 0, sizeof(*cpl));
    cmd.version = 1U;
    cmd.opcode = opcode;
    cmd.req_id = mem_service_payload_get_u64(payload,
                                             "backend_request_id",
                                             record->version);
    cmd.source_cna = mem_service_payload_get_u32(payload,
                                                 "backend_source_cna",
                                                 0U);
    cmd.target_ssd_cna = mem_service_payload_get_u32(
        payload,
        "backend_device_cna",
        record->object_backend_device_cna);
    cmd.flags = mem_service_payload_get_u32(payload, "backend_flags", 0U);
    cmd.block_ref.block_hi = record->object_backend_block_hi;
    cmd.block_ref.block_lo = record->object_backend_block_lo;
    cmd.block_ref.version = record->object_backend_block_version;
    cmd.block_ref.offset = record->object_backend_block_offset;
    cmd.block_ref.bytes = record->object_backend_block_bytes;
    cmd.block_ref.checksum64 = record->object_backend_block_checksum;
    if (!mem_service_payload_get_ub_ssd_buffer_desc(payload, &cmd.buffer)) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    device_path[0] = '\0';
    (void)mem_service_payload_get_string(payload,
                                         "backend_device_path",
                                         device_path,
                                         sizeof(device_path));
    if (device_path[0] == '\0') {
        snprintf(device_path, sizeof(device_path), "%s",
                 MEM_SERVICE_UB_SSD_DEFAULT_DEVICE);
    }
    fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENODEV || errno == ENOTTY) {
            return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
        }
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    rc = ioctl(fd, MEM_SERVICE_UB_SSD_SUBMIT, &cmd);
    if (rc == 0) {
        rc = ioctl(fd, MEM_SERVICE_UB_SSD_WAIT, cpl);
    }
    close(fd);
    if (rc != 0) {
        if (errno == ENOTTY || errno == ENODEV) {
            return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
        }
        return MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }
    return mem_service_ub_ssd_status_to_wire((int32_t)cpl->status);
#endif
}

static enum mem_service_wire_status mem_service_write_ub_ssd_backend_block(
    const char *payload,
    struct mem_service_record *record)
{
    struct mem_service_ub_ssd_cpl_v1 cpl;
    enum mem_service_wire_status status;

    if (record == NULL) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    status = mem_service_ub_ssd_submit(payload,
                                       MEM_SERVICE_UB_SSD_OP_BLOCK_WRITE,
                                       record,
                                       &cpl);
    if (status != MEM_SERVICE_WIRE_STATUS_OK) {
        return status;
    }
    record->object_backend_block_hi = cpl.committed_ref.block_hi;
    record->object_backend_block_lo = cpl.committed_ref.block_lo;
    record->object_backend_block_version = cpl.committed_ref.version;
    record->object_backend_block_offset = cpl.committed_ref.offset;
    record->object_backend_block_bytes = cpl.committed_ref.bytes;
    record->object_backend_block_checksum = cpl.committed_ref.checksum64;
    record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_UB_SSD_GSVA_BLOCK;
    record->object_backing_offset = cpl.committed_ref.offset;
    record->object_backing_len = cpl.committed_ref.bytes;
    record->object_payload_checksum = cpl.committed_ref.checksum64;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static enum mem_service_wire_status mem_service_read_ub_ssd_backend_block(
    const char *payload,
    const struct mem_service_record *record,
    struct mem_service_ub_ssd_cpl_v1 *cpl)
{
    if (record == NULL ||
        record->object_backend_kind != MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    return mem_service_ub_ssd_submit(payload,
                                     MEM_SERVICE_UB_SSD_OP_BLOCK_READ,
                                     record,
                                     cpl);
}

static enum mem_service_wire_status mem_service_apply_ub_ssd_backend_ref(
    const char *payload,
    const char *payload_inline,
    const char *payload_path,
    struct mem_service_record *record)
{
    uint64_t block_hi = 0;
    uint64_t block_lo = 0;
    uint64_t block_bytes = 0;
    uint64_t block_checksum = 0;
    bool write_requested = false;

    if (!mem_service_payload_selects_ub_ssd_backend(payload)) {
        return MEM_SERVICE_WIRE_STATUS_OK;
    }
    write_requested = mem_service_payload_ub_ssd_write_requested(payload);
    if ((payload_inline != NULL && payload_inline[0] != '\0') ||
        (payload_path != NULL && payload_path[0] != '\0')) {
        return MEM_SERVICE_WIRE_STATUS_UNSUPPORTED;
    }
    if (record == NULL ||
        !mem_service_payload_get_u64_checked(payload, "backend_block_hi", &block_hi) ||
        !mem_service_payload_get_u64_checked(payload, "backend_block_lo", &block_lo)) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (!write_requested &&
        (!mem_service_payload_get_u64_checked(payload,
                                              "backend_block_bytes",
                                              &block_bytes) ||
         !mem_service_payload_get_u64_checked(payload,
                                              "backend_block_checksum",
                                              &block_checksum))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (!write_requested && (block_bytes == 0 || block_checksum == 0)) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (write_requested) {
        block_bytes = mem_service_payload_get_u64(payload,
                                                  "backend_block_bytes",
                                                  mem_service_payload_get_u64(
                                                      payload,
                                                      "backend_buffer_bytes",
                                                      0U));
        block_checksum = mem_service_payload_get_u64(payload,
                                                     "backend_block_checksum",
                                                     0U);
    }
    record->object_backend_kind = MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA;
    record->object_backend_node =
        mem_service_payload_get_u32(payload, "backend_node", record->object_owner_node);
    record->object_backend_device_cna =
        mem_service_payload_get_u32(payload, "backend_device_cna", 0);
    record->object_backend_flags = mem_service_payload_get_u32(payload, "backend_flags", 0);
    record->object_backend_block_hi = block_hi;
    record->object_backend_block_lo = block_lo;
    record->object_backend_block_version =
        mem_service_payload_get_u64(payload, "backend_block_version", record->version);
    record->object_backend_block_offset =
        mem_service_payload_get_u64(payload, "backend_block_offset", 0);
    record->object_backend_block_bytes = block_bytes;
    record->object_backend_block_checksum = block_checksum;
    record->object_payload_kind = MEM_SERVICE_PAYLOAD_KIND_UB_SSD_GSVA_BLOCK;
    record->object_backing_offset = record->object_backend_block_offset;
    record->object_backing_len = block_bytes;
    record->object_payload_checksum = block_checksum;
    return MEM_SERVICE_WIRE_STATUS_OK;
}

static void mem_service_format_record_payload(const struct mem_service_record *record,
                                              char *out,
                                              size_t out_len)
{
    if (record->object_backend_kind == MEM_SERVICE_OBJECT_BACKEND_LEGACY_PAYLOAD) {
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
        return;
    }
    snprintf(out,
             out_len,
             "key=%s\nkind=%u\nrequest_id=%s\nprefix_group=%s\ngroup_id=%s\n"
             "session_id=%s\nmodel_key=%s\nartifact_kind=%s\nartifact_id=%s\n"
             "block_hash=%s\nplacement_node=%u\nplacement_level=%u\n"
             "hot_segment_id=%" PRIu64 "\nstate=%s\nversion=%" PRIu64 "\n"
             "last_result_segment=%" PRIu64 "\nobject_owner_node=%u\n"
             "object_payload_kind=%u\nobject_backing_offset=%" PRIu64 "\n"
             "object_backing_len=%" PRIu64 "\nobject_payload_checksum=%" PRIu64 "\n"
             "object_backend=%s\nobject_backend_kind=%u\n"
             "object_backend_node=%u\nobject_backend_device_cna=%u\n"
             "object_backend_flags=%u\n"
             "object_backend_block_hi=%" PRIu64 "\n"
             "object_backend_block_lo=%" PRIu64 "\n"
             "object_backend_block_version=%" PRIu64 "\n"
             "object_backend_block_offset=%" PRIu64 "\n"
             "object_backend_block_bytes=%" PRIu64 "\n"
             "object_backend_block_checksum=%" PRIu64 "\n",
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
             record->object_payload_checksum,
             mem_service_object_backend_name(record->object_backend_kind),
             record->object_backend_kind,
             record->object_backend_node,
             record->object_backend_device_cna,
             record->object_backend_flags,
             record->object_backend_block_hi,
             record->object_backend_block_lo,
             record->object_backend_block_version,
             record->object_backend_block_offset,
             record->object_backend_block_bytes,
             record->object_backend_block_checksum);
}

static void mem_service_format_inspect_record_payload(
    const struct mem_service_record *record,
    char *out,
    size_t out_len)
{
    if (record->object_backend_kind == MEM_SERVICE_OBJECT_BACKEND_LEGACY_PAYLOAD) {
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
        return;
    }
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
             "object_backend=%s\nobject_backend_kind=%u\n"
             "object_backend_node=%u\nobject_backend_device_cna=%u\n"
             "object_backend_flags=%u\n"
             "object_backend_block_hi=%" PRIu64 "\n"
             "object_backend_block_lo=%" PRIu64 "\n"
             "object_backend_block_version=%" PRIu64 "\n"
             "object_backend_block_offset=%" PRIu64 "\n"
             "object_backend_block_bytes=%" PRIu64 "\n"
             "object_backend_block_checksum=%" PRIu64 "\n"
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
             mem_service_object_backend_name(record->object_backend_kind),
             record->object_backend_kind,
             record->object_backend_node,
             record->object_backend_device_cna,
             record->object_backend_flags,
             record->object_backend_block_hi,
             record->object_backend_block_lo,
             record->object_backend_block_version,
             record->object_backend_block_offset,
             record->object_backend_block_bytes,
             record->object_backend_block_checksum,
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
    block_status = mem_service_apply_ub_ssd_backend_ref(payload,
                                                        payload_inline,
                                                        payload_path,
                                                        &next);
    if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return block_status;
    }
    if (next.object_backend_kind == MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA &&
        mem_service_payload_ub_ssd_write_requested(payload)) {
        block_status = mem_service_write_ub_ssd_backend_block(payload, &next);
        if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
            return block_status;
        }
    }
    if (next.object_backend_kind != MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA &&
        (payload_inline[0] != '\0' || payload_path[0] != '\0')) {
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
    struct mem_service_ub_ssd_cpl_v1 read_cpl;
    bool read_performed = false;

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
    if (record.object_backend_kind == MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA &&
        mem_service_payload_ub_ssd_read_requested(payload)) {
        block_status = mem_service_read_ub_ssd_backend_block(payload,
                                                             &record,
                                                             &read_cpl);
        if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
            return block_status;
        }
        read_performed = true;
    }
    mem_service_format_record_payload(&record, response, response_len);
    if (read_performed) {
        size_t used = strlen(response);
        int written;

        if (used >= response_len) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
        written = snprintf(response + used,
                           response_len - used,
                           "object_backend_read_bytes=%" PRIu64 "\n"
                           "object_backend_read_checksum=%" PRIu64 "\n"
                           "object_backend_read_version=%" PRIu64 "\n",
                           read_cpl.bytes_read,
                           read_cpl.checksum64,
                           read_cpl.committed_ref.version);
        if (written < 0 || (size_t)written >= response_len - used) {
            return MEM_SERVICE_WIRE_STATUS_INTERNAL;
        }
    }
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
    block_status = mem_service_apply_ub_ssd_backend_ref(payload,
                                                        payload_inline,
                                                        payload_path,
                                                        &next);
    if (block_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return block_status;
    }
    if (next.object_backend_kind != MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA &&
        (payload_inline[0] != '\0' || payload_path[0] != '\0')) {
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
    uint32_t expected_owner;
    uint64_t expected_version;
    uint64_t expected_checksum;
    enum mem_service_wire_status block_status;

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    }
    if (mem_service_get_record(svc, key, &record) != 0 || record.kind != record_kind) {
        return MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    }
    if (svc->enforce_expected_context) {
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
        if (mem_service_payload_get_u32_checked(payload,
                                                "expected_owner",
                                                &expected_owner) &&
            record.object_owner_node != expected_owner) {
            return MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING;
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
                                         "%s\nstore_schema_version=%d\n"
                                         "record_count=%zu\n"
                                         "audit_next_sequence=%" PRIu64 "\n"
                                         "audit_event_count=%" PRIu64 "\n",
                                         MEM_SERVICE_STORE_MAGIC,
                                         MEM_SERVICE_STORE_SCHEMA_VERSION,
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
             "store_schema_version=%d\n"
             "record_count=%zu\n"
             "start_index=%zu\n"
             "next_index=%zu\n"
             "records_emitted=%zu\n"
             "complete=%u\n"
             "%s",
             MEM_SERVICE_STORE_MAGIC,
             MEM_SERVICE_STORE_SCHEMA_VERSION,
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

static bool mem_service_apply_audit_retention(struct mem_service *svc,
                                              uint64_t max_audit_events)
{
    uint64_t first_kept_sequence;
    uint64_t retained = 0;
    size_t i;
    bool pruned = false;

    if (svc == NULL || max_audit_events == 0 ||
        max_audit_events >= MEM_SERVICE_MAX_AUDIT_EVENTS ||
        svc->audit_event_count <= max_audit_events ||
        svc->audit_next_sequence == 0) {
        return false;
    }
    first_kept_sequence = svc->audit_next_sequence > max_audit_events
                              ? svc->audit_next_sequence - max_audit_events
                              : 1U;
    for (i = 0; i < MEM_SERVICE_MAX_AUDIT_EVENTS; ++i) {
        struct mem_service_audit_event *event = &svc->audit_events[i];

        if (!event->in_use) {
            continue;
        }
        if (event->sequence < first_kept_sequence) {
            memset(event, 0, sizeof(*event));
            pruned = true;
            continue;
        }
        retained += 1U;
    }
    svc->audit_event_count = retained;
    return pruned;
}

static bool mem_service_record_is_checkpoint(const struct mem_service_record *record)
{
    return record != NULL &&
           record->in_use &&
           record->kind == MEM_SERVICE_RECORD_TRAINING_ARTIFACT &&
           strcmp(record->artifact_kind, "checkpoint") == 0;
}

static uint64_t mem_service_count_checkpoint_records(const struct mem_service *svc)
{
    uint64_t count = 0;
    size_t i;

    if (svc == NULL) {
        return 0;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (mem_service_record_is_checkpoint(&svc->records[i])) {
            count += 1U;
        }
    }
    return count;
}

static size_t mem_service_find_oldest_checkpoint_record_index(
    const struct mem_service *svc)
{
    size_t oldest = MEM_SERVICE_MAX_RECORDS;
    size_t i;

    if (svc == NULL) {
        return oldest;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *record = &svc->records[i];

        if (!mem_service_record_is_checkpoint(record)) {
            continue;
        }
        if (oldest == MEM_SERVICE_MAX_RECORDS ||
            record->version < svc->records[oldest].version ||
            (record->version == svc->records[oldest].version &&
             record->object_publish_monotonic_ms <
                 svc->records[oldest].object_publish_monotonic_ms)) {
            oldest = i;
        }
    }
    return oldest;
}

static bool mem_service_record_matches_retention_kind(
    const struct mem_service_record *record,
    uint32_t retained_record_kind,
    bool retained_record_tenant_enabled,
    uint32_t retained_record_tenant)
{
    return record != NULL && record->in_use &&
           (retained_record_kind == 0U ||
            (uint32_t)record->kind == retained_record_kind) &&
           (!retained_record_tenant_enabled ||
            record->object_owner_node == retained_record_tenant);
}

static uint64_t mem_service_count_retained_kind_records(
    const struct mem_service *svc,
    uint32_t retained_record_kind,
    bool retained_record_tenant_enabled,
    uint32_t retained_record_tenant)
{
    uint64_t count = 0;
    size_t i;

    if (svc == NULL) {
        return 0;
    }
    if (retained_record_kind == 0U && !retained_record_tenant_enabled) {
        return (uint64_t)svc->record_count;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (mem_service_record_matches_retention_kind(&svc->records[i],
                                                      retained_record_kind,
                                                      retained_record_tenant_enabled,
                                                      retained_record_tenant)) {
            count += 1U;
        }
    }
    return count;
}

static size_t mem_service_find_oldest_record_index(const struct mem_service *svc,
                                                   uint32_t retained_record_kind,
                                                   bool retained_record_tenant_enabled,
                                                   uint32_t retained_record_tenant)
{
    size_t oldest = MEM_SERVICE_MAX_RECORDS;
    size_t i;

    if (svc == NULL) {
        return oldest;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *record = &svc->records[i];

        if (!mem_service_record_matches_retention_kind(record,
                                                       retained_record_kind,
                                                       retained_record_tenant_enabled,
                                                       retained_record_tenant)) {
            continue;
        }
        if (oldest == MEM_SERVICE_MAX_RECORDS ||
            record->version < svc->records[oldest].version ||
            (record->version == svc->records[oldest].version &&
             record->object_publish_monotonic_ms <
                 svc->records[oldest].object_publish_monotonic_ms)) {
            oldest = i;
        }
    }
    return oldest;
}

static void mem_service_prune_idempotency_for_record_key(struct mem_service *svc,
                                                         const char *record_key)
{
    size_t i;

    if (svc == NULL || record_key == NULL || record_key[0] == '\0') {
        return;
    }
    for (i = 0; i < MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS; ++i) {
        struct mem_service_idempotency_record *idem = &svc->idempotency_records[i];
        char response_key[96];

        if (!idem->in_use) {
            continue;
        }
        response_key[0] = '\0';
        if (mem_service_payload_get_string(idem->response,
                                           "key",
                                           response_key,
                                           sizeof(response_key)) &&
            strcmp(response_key, record_key) == 0) {
            memset(idem, 0, sizeof(*idem));
        }
    }
}

static bool mem_service_apply_checkpoint_retention(struct mem_service *svc,
                                                   uint64_t max_checkpoint_records,
                                                   const char *storage_root,
                                                   uint64_t *payload_gc_out)
{
    bool pruned = false;

    if (svc == NULL || max_checkpoint_records == 0) {
        return false;
    }
    while (mem_service_count_checkpoint_records(svc) > max_checkpoint_records) {
        size_t oldest = mem_service_find_oldest_checkpoint_record_index(svc);
        char key[96];

        if (oldest == MEM_SERVICE_MAX_RECORDS) {
            break;
        }
        snprintf(key, sizeof(key), "%s", svc->records[oldest].key);
        (void)mem_service_gc_payload_block_if_orphaned(svc,
                                                       oldest,
                                                       storage_root,
                                                       payload_gc_out);
        memset(&svc->records[oldest], 0, sizeof(svc->records[oldest]));
        if (svc->record_count > 0) {
            svc->record_count -= 1U;
        }
        mem_service_prune_idempotency_for_record_key(svc, key);
        pruned = true;
    }
    return pruned;
}

static bool mem_service_apply_record_retention(struct mem_service *svc,
                                               uint64_t max_retained_records,
                                               uint64_t max_retained_record_age_ms,
                                               uint32_t retained_record_kind,
                                               bool retained_record_tenant_enabled,
                                               uint32_t retained_record_tenant,
                                               const char *storage_root,
                                               uint64_t *payload_gc_out)
{
    bool pruned = false;

    if (svc == NULL ||
        (max_retained_records == 0 && max_retained_record_age_ms == 0)) {
        return false;
    }
    while (mem_service_count_retained_kind_records(
               svc,
               retained_record_kind,
               retained_record_tenant_enabled,
               retained_record_tenant) >
           max_retained_records &&
           max_retained_records > 0) {
        size_t oldest = mem_service_find_oldest_record_index(
            svc,
            retained_record_kind,
            retained_record_tenant_enabled,
            retained_record_tenant);
        char key[96];

        if (oldest == MEM_SERVICE_MAX_RECORDS) {
            break;
        }
        snprintf(key, sizeof(key), "%s", svc->records[oldest].key);
        (void)mem_service_gc_payload_block_if_orphaned(svc,
                                                       oldest,
                                                       storage_root,
                                                       payload_gc_out);
        memset(&svc->records[oldest], 0, sizeof(svc->records[oldest]));
        if (svc->record_count > 0) {
            svc->record_count -= 1U;
        }
        mem_service_prune_idempotency_for_record_key(svc, key);
        pruned = true;
    }
    if (max_retained_record_age_ms > 0) {
        uint64_t now_ms = mem_service_wall_clock_ms();
        size_t i;

        for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
            char key[96];
            struct mem_service_record *record = &svc->records[i];

            if (!mem_service_record_matches_retention_kind(
                    record,
                    retained_record_kind,
                    retained_record_tenant_enabled,
                    retained_record_tenant) ||
                record->object_publish_monotonic_ms == 0 ||
                record->object_publish_monotonic_ms > now_ms ||
                now_ms - record->object_publish_monotonic_ms <=
                    max_retained_record_age_ms) {
                continue;
            }
            snprintf(key, sizeof(key), "%s", record->key);
            (void)mem_service_gc_payload_block_if_orphaned(svc,
                                                           i,
                                                           storage_root,
                                                           payload_gc_out);
            memset(record, 0, sizeof(*record));
            if (svc->record_count > 0) {
                svc->record_count -= 1U;
            }
            mem_service_prune_idempotency_for_record_key(svc, key);
            pruned = true;
        }
    }
    return pruned;
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

static enum mem_service_wire_status mem_service_handle_operation_with_limits(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload,
    char *response,
    size_t response_len,
    const char *store_path,
    const char *storage_root,
    const struct mem_service_daemon_limits *limits)
{
    uint64_t start_ms = mem_service_monotonic_ms();
    uint64_t end_ms;
    uint64_t latency_ms;
    struct mem_service_idempotency_record *pending_idempotency = NULL;
    const struct mem_service_audit_event *audit_event = NULL;
    bool idempotency_handled = false;
    bool audit_appended = false;
    bool audit_retention_pruned = false;
    bool checkpoint_retention_pruned = false;
    bool record_retention_pruned = false;
    enum mem_service_wire_status status =
        mem_service_try_idempotency_replay(svc,
                                           operation,
                                           payload,
                                           response,
                                           response_len,
                                           &pending_idempotency,
                                           &idempotency_handled);

    if (!idempotency_handled) {
        uint64_t new_records =
            mem_service_estimate_new_record_count(svc, operation, payload);

        if (limits != NULL && limits->max_records > 0 && new_records > 0 &&
            (new_records == UINT64_MAX ||
             (uint64_t)svc->record_count >= limits->max_records ||
             new_records > limits->max_records - (uint64_t)svc->record_count)) {
            status = MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
            snprintf(response,
                     response_len,
                     "status=capacity_exceeded\nquota=max_records\nmax_records=%" PRIu64
                     "\nrecord_count=%zu\nrequired_records=%" PRIu64 "\n",
                     limits->max_records,
                     svc->record_count,
                     new_records == UINT64_MAX ? 0U : new_records);
        } else {
            status = mem_service_dispatch_operation(svc,
                                                    operation,
                                                    payload,
                                                    response,
                                                    response_len,
                                                    storage_root);
            if (status == MEM_SERVICE_WIRE_STATUS_OK &&
                operation == MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT &&
                limits != NULL &&
                limits->max_checkpoint_records > 0) {
                checkpoint_retention_pruned =
                    mem_service_apply_checkpoint_retention(
                        svc,
                        limits->max_checkpoint_records,
                        storage_root,
                        NULL);
            }
            if (status == MEM_SERVICE_WIRE_STATUS_OK &&
                mem_service_operation_mutates(operation, payload) &&
                limits != NULL &&
                (limits->max_retained_records > 0 ||
                 limits->max_retained_record_age_ms > 0)) {
                record_retention_pruned =
                    mem_service_apply_record_retention(
                        svc,
                        limits->max_retained_records,
                        limits->max_retained_record_age_ms,
                        limits->max_retained_record_kind,
                        limits->max_retained_record_tenant_enabled,
                        limits->max_retained_record_tenant,
                        storage_root,
                        NULL);
            }
        }
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
    if (audit_appended && limits != NULL && limits->max_audit_events > 0) {
        audit_retention_pruned =
            mem_service_apply_audit_retention(svc, limits->max_audit_events);
    }
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

    if (status == MEM_SERVICE_WIRE_STATUS_OK &&
        mem_service_operation_mutates(operation, payload) &&
        mem_service_compact_journal(store_path) != 0) {
        fprintf(stderr,
                "mem_service journal-compaction: compact_journal failed operation=%u\n",
                (unsigned int)operation);
    }
    if (audit_retention_pruned && store_path != NULL &&
        mem_service_compact_journal_now(store_path) != 0) {
        fprintf(stderr,
                "mem_service retention: compact_journal failed operation=%u\n",
                (unsigned int)operation);
    }
    if (checkpoint_retention_pruned && store_path != NULL &&
        (mem_service_save_store(svc, store_path) != 0 ||
         mem_service_compact_journal_now(store_path) != 0)) {
        fprintf(stderr,
                "mem_service checkpoint-retention: durable sync failed operation=%u\n",
                (unsigned int)operation);
    }
    if (record_retention_pruned && store_path != NULL &&
        (mem_service_save_store(svc, store_path) != 0 ||
         mem_service_compact_journal_now(store_path) != 0)) {
        fprintf(stderr,
                "mem_service record-retention: durable sync failed operation=%u\n",
                (unsigned int)operation);
    }

    end_ms = mem_service_monotonic_ms();
    latency_ms = end_ms >= start_ms ? end_ms - start_ms : 0;
    mem_service_record_operation_metrics(svc, operation, status, latency_ms);
    return status;
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
    return mem_service_handle_operation_with_limits(svc,
                                                    operation,
                                                    payload,
                                                    response,
                                                    response_len,
                                                    store_path,
                                                    storage_root,
                                                    NULL);
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

static uint64_t mem_service_estimate_new_key_record_count(struct mem_service *svc,
                                                          const char *payload)
{
    struct mem_service_record shape;
    struct mem_service_record *record;
    char key[sizeof(shape.key)];

    if (!mem_service_payload_get_string(payload, "key", key, sizeof(key))) {
        return UINT64_MAX;
    }
    record = mem_service_find_record(svc, key);
    return record == NULL ? 1U : 0U;
}

static uint64_t mem_service_estimate_new_kv_record_count(struct mem_service *svc,
                                                         const char *payload)
{
    struct mem_service_record shape;
    char block_hash[sizeof(shape.block_hash)];
    char block_key[96];

    if (!mem_service_payload_get_string(payload,
                                        "block_hash",
                                        block_hash,
                                        sizeof(block_hash))) {
        return UINT64_MAX;
    }
    mem_service_build_block_key_from_hash(block_hash, block_key, sizeof(block_key));
    return mem_service_find_record(svc, block_key) == NULL ? 1U : 0U;
}

static uint64_t mem_service_estimate_new_prefix_record_count(struct mem_service *svc,
                                                             const char *payload)
{
    struct mem_service_record shape;
    char request_id[sizeof(shape.request_id)];
    char prefix_group[sizeof(shape.prefix_group)];
    char prefix_key[96];
    uint64_t new_records = 0;
    uint64_t new_kv_records;

    if (!mem_service_payload_get_string(payload,
                                        "request_id",
                                        request_id,
                                        sizeof(request_id)) ||
        !mem_service_payload_get_string(payload,
                                        "prefix_group",
                                        prefix_group,
                                        sizeof(prefix_group))) {
        return UINT64_MAX;
    }
    mem_service_build_prefix_key_from_parts(request_id,
                                            prefix_group,
                                            prefix_key,
                                            sizeof(prefix_key));
    if (mem_service_find_record(svc, prefix_key) == NULL) {
        new_records += 1U;
    }
    new_kv_records = mem_service_estimate_new_kv_record_count(svc, payload);
    if (new_kv_records == UINT64_MAX) {
        return UINT64_MAX;
    }
    return new_records + new_kv_records;
}

static uint64_t mem_service_estimate_new_record_count(
    struct mem_service *svc,
    enum mem_service_wire_operation operation,
    const char *payload)
{
    switch (operation) {
    case MEM_SERVICE_WIRE_OP_PUT_OBJECT:
    case MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF:
    case MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT:
    case MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT:
        return mem_service_estimate_new_key_record_count(svc, payload);
    case MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT:
        return mem_service_estimate_new_kv_record_count(svc, payload);
    case MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY:
        return mem_service_estimate_new_prefix_record_count(svc, payload);
    default:
        return mem_service_operation_mutates(operation, payload) ? UINT64_MAX : 0U;
    }
}

static bool mem_service_request_exceeds_payload_limit(
    uint32_t payload_len,
    const struct mem_service_daemon_limits *limits)
{
    return limits != NULL && limits->max_payload_bytes > 0 &&
           payload_len > limits->max_payload_bytes;
}

int mem_service_run_runtime_quota_fixture_check(void)
{
    struct mem_service svc;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status first_status;
    enum mem_service_wire_status update_status;
    enum mem_service_wire_status second_status;

    limits.max_records = 1U;
    limits.max_payload_bytes = 24U;
    limits.max_audit_events = 0;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service runtime-quota-fixtures: init failed\n");
        return 1;
    }
    first_status = mem_service_handle_operation_with_limits(
        &svc,
        MEM_SERVICE_WIRE_OP_PUT_OBJECT,
        "key=runtime-quota-1\nversion=1\nchecksum=11\nbacking_len=8\n",
        response,
        sizeof(response),
        NULL,
        NULL,
        &limits);
    update_status = mem_service_handle_operation_with_limits(
        &svc,
        MEM_SERVICE_WIRE_OP_PUT_OBJECT,
        "key=runtime-quota-1\nversion=2\nchecksum=12\nbacking_len=8\n",
        response,
        sizeof(response),
        NULL,
        NULL,
        &limits);
    second_status = mem_service_handle_operation_with_limits(
        &svc,
        MEM_SERVICE_WIRE_OP_PUT_OBJECT,
        "key=runtime-quota-2\nversion=1\nchecksum=22\nbacking_len=8\n",
        response,
        sizeof(response),
        NULL,
        NULL,
        &limits);
    if (first_status != MEM_SERVICE_WIRE_STATUS_OK ||
        update_status != MEM_SERVICE_WIRE_STATUS_OK ||
        second_status != MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED ||
        strstr(response, "quota=max_records\n") == NULL ||
        svc.record_count != 1U ||
        svc.metrics.capacity_exceeded_count != 1U) {
        fprintf(stderr,
                "mem_service runtime-quota-fixtures: max_records admission mismatch\n");
        return 1;
    }
    if (!mem_service_request_exceeds_payload_limit(25U, &limits) ||
        mem_service_request_exceeds_payload_limit(24U, &limits) ||
        mem_service_request_exceeds_payload_limit(1024U, NULL)) {
        fprintf(stderr,
                "mem_service runtime-quota-fixtures: max_payload_bytes admission mismatch\n");
        return 1;
    }
    printf("mem_service runtime-quota-fixtures: status=ok "
           "runtime_quota=max-records+max-payload-bytes "
           "max_records=%" PRIu64 " max_payload_bytes=%" PRIu64
           " record_count=%zu capacity_exceeded=%" PRIu64 "\n",
           limits.max_records,
           limits.max_payload_bytes,
           svc.record_count,
           svc.metrics.capacity_exceeded_count);
    return 0;
}

int mem_service_run_retention_fixture_check(void)
{
    struct mem_service svc;
    struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[160];
    char journal_path[sizeof(store_path) + 16U];
    size_t i;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_retention_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service retention-fixtures: journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    memset(&limits, 0, sizeof(limits));
    limits.max_audit_events = 2U;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service retention-fixtures: init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    for (i = 0; i < 4U; ++i) {
        char payload[160];
        enum mem_service_wire_status status;

        snprintf(payload,
                 sizeof(payload),
                 "key=retention-object-%zu\nversion=%zu\nchecksum=%zu\n"
                 "backing_len=8\n",
                 i + 1U,
                 i + 1U,
                 100U + i);
        status = mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_PUT_OBJECT,
            payload,
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service retention-fixtures: put failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name((uint32_t)status));
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    if (svc.record_count != 4U ||
        svc.audit_event_count != 2U ||
        mem_service_audit_first_sequence(&svc) != 3U ||
        mem_service_find_audit_sequence(&svc, 1U) != NULL ||
        mem_service_find_audit_sequence(&svc, 2U) != NULL ||
        mem_service_find_audit_sequence(&svc, 3U) == NULL ||
        mem_service_find_audit_sequence(&svc, 4U) == NULL) {
        fprintf(stderr, "mem_service retention-fixtures: in-memory gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0) {
        fprintf(stderr, "mem_service retention-fixtures: durable load failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (recovered.record_count != 4U ||
        recovered.audit_event_count != 2U ||
        mem_service_audit_first_sequence(&recovered) != 3U ||
        mem_service_find_audit_sequence(&recovered, 1U) != NULL ||
        mem_service_find_audit_sequence(&recovered, 2U) != NULL ||
        mem_service_find_audit_sequence(&recovered, 3U) == NULL ||
        mem_service_find_audit_sequence(&recovered, 4U) == NULL) {
        fprintf(stderr, "mem_service retention-fixtures: durable gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_file_contains(journal_path, "retention-object-1") ||
        mem_service_file_contains(journal_path, "retention-object-2")) {
        fprintf(stderr, "mem_service retention-fixtures: journal gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service retention-fixtures: status=ok "
           "retention_policy=audit-log-limit max_audit_events=%" PRIu64
           " retained_events=%" PRIu64 " first_sequence=%" PRIu64
           " record_count=%zu durable_reload=1 journal_gc=1\n",
           limits.max_audit_events,
           recovered.audit_event_count,
           mem_service_audit_first_sequence(&recovered),
           recovered.record_count);
    return 0;
}

int mem_service_run_checkpoint_retention_fixture_check(void)
{
    struct mem_service svc;
    struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[176];
    char journal_path[sizeof(store_path) + 16U];
    size_t i;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_checkpoint_retention_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service checkpoint-retention-fixtures: journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    memset(&limits, 0, sizeof(limits));
    limits.max_checkpoint_records = 2U;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service checkpoint-retention-fixtures: init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    for (i = 0; i < 4U; ++i) {
        char payload[384];
        enum mem_service_wire_status status;

        snprintf(payload,
                 sizeof(payload),
                 "key=training/run/checkpoint-%zu\n"
                 "session_id=train-session\n"
                 "model_key=model-a\n"
                 "artifact_kind=checkpoint\n"
                 "artifact_id=checkpoint-%zu\n"
                 "owner=7\n"
                 "version=%zu\n"
                 "checksum=%zu\n"
                 "idempotency_key=checkpoint-retention-idem-%zu\n",
                 i + 1U,
                 i + 1U,
                 i + 1U,
                 700U + i,
                 i + 1U);
        status = mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            payload,
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service checkpoint-retention-fixtures: checkpoint put failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name((uint32_t)status));
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            "key=training/run/gradient-1\n"
            "session_id=train-session\n"
            "model_key=model-a\n"
            "artifact_kind=gradient\n"
            "artifact_id=gradient-1\n"
            "owner=7\n"
            "version=1\n"
            "checksum=900\n"
            "idempotency_key=checkpoint-retention-gradient-idem\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits) != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service checkpoint-retention-fixtures: gradient put failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (svc.record_count != 3U ||
        mem_service_count_checkpoint_records(&svc) != 2U ||
        mem_service_find_record(&svc, "training/run/checkpoint-1") != NULL ||
        mem_service_find_record(&svc, "training/run/checkpoint-2") != NULL ||
        mem_service_find_record(&svc, "training/run/checkpoint-3") == NULL ||
        mem_service_find_record(&svc, "training/run/checkpoint-4") == NULL ||
        mem_service_find_record(&svc, "training/run/gradient-1") == NULL ||
        mem_service_find_idempotency_record(&svc, "checkpoint-retention-idem-1") != NULL ||
        mem_service_find_idempotency_record(&svc, "checkpoint-retention-idem-2") != NULL) {
        fprintf(stderr,
                "mem_service checkpoint-retention-fixtures: in-memory gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
            "key=training/run/checkpoint-1\n"
            "expected_artifact_kind=checkpoint\n",
            response,
            sizeof(response),
            NULL,
            NULL,
            &limits) != MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
        fprintf(stderr,
                "mem_service checkpoint-retention-fixtures: old checkpoint query mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0) {
        fprintf(stderr, "mem_service checkpoint-retention-fixtures: durable load failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (recovered.record_count != 3U ||
        mem_service_count_checkpoint_records(&recovered) != 2U ||
        mem_service_find_record(&recovered, "training/run/checkpoint-1") != NULL ||
        mem_service_find_record(&recovered, "training/run/checkpoint-2") != NULL ||
        mem_service_find_record(&recovered, "training/run/checkpoint-3") == NULL ||
        mem_service_find_record(&recovered, "training/run/checkpoint-4") == NULL ||
        mem_service_find_record(&recovered, "training/run/gradient-1") == NULL ||
        mem_service_find_idempotency_record(&recovered,
                                            "checkpoint-retention-idem-1") != NULL ||
        mem_service_find_idempotency_record(&recovered,
                                            "checkpoint-retention-idem-2") != NULL) {
        fprintf(stderr,
                "mem_service checkpoint-retention-fixtures: durable gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_file_contains(journal_path, "training/run/checkpoint-1") ||
        mem_service_file_contains(journal_path, "training/run/checkpoint-2")) {
        fprintf(stderr,
                "mem_service checkpoint-retention-fixtures: journal gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service checkpoint-retention-fixtures: status=ok "
           "checkpoint_retention=latest max_checkpoint_records=%" PRIu64
           " retained_checkpoints=%" PRIu64
           " record_count=%zu non_checkpoint_retained=1 durable_reload=1 "
           "idempotency_gc=1 journal_gc=1\n",
           limits.max_checkpoint_records,
           mem_service_count_checkpoint_records(&recovered),
           recovered.record_count);
    return 0;
}

int mem_service_run_payload_gc_fixture_check(void)
{
    static const char *payloads[4] = {
        "orphan-checkpoint-payload",
        "shared-checkpoint-payload",
        "shared-checkpoint-payload",
        "latest-checkpoint-payload",
    };
    struct mem_service svc;
    struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char storage_root[176];
    char store_path[224];
    char journal_path[sizeof(store_path) + 16U];
    char blocks_dir[224];
    char catalog_dir[224];
    char quarantine_dir[224];
    char orphan_block[256];
    char shared_block[256];
    char latest_block[256];
    uint64_t checksums[4];
    size_t i;

    snprintf(storage_root,
             sizeof(storage_root),
             "/tmp/linqu_mem_service_payload_gc_fixture_%ld",
             (long)getpid());
    if (mem_service_join_path(blocks_dir,
                              sizeof(blocks_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(catalog_dir,
                              sizeof(catalog_dir),
                              storage_root,
                              "catalog") != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        mem_service_make_catalog_path(storage_root,
                                      "store.snapshot",
                                      store_path,
                                      sizeof(store_path)) != 0 ||
        mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service payload-gc-fixtures: path setup failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    rmdir(blocks_dir);
    rmdir(catalog_dir);
    rmdir(quarantine_dir);
    rmdir(storage_root);
    if (mem_service_prepare_durable_catalog_layout(storage_root) != 0) {
        fprintf(stderr, "mem_service payload-gc-fixtures: storage setup failed\n");
        return 1;
    }
    memset(&limits, 0, sizeof(limits));
    limits.max_checkpoint_records = 2U;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service payload-gc-fixtures: init failed\n");
        rmdir(blocks_dir);
        rmdir(catalog_dir);
        rmdir(quarantine_dir);
        rmdir(storage_root);
        return 1;
    }
    for (i = 0U; i < 4U; ++i) {
        char request[640];
        enum mem_service_wire_status status;

        checksums[i] = mem_service_checksum_bytes((const uint8_t *)payloads[i],
                                                  (uint64_t)strlen(payloads[i]));
        snprintf(request,
                 sizeof(request),
                 "key=training/gc/checkpoint-%zu\n"
                 "session_id=gc-session\n"
                 "model_key=gc-model\n"
                 "artifact_kind=checkpoint\n"
                 "artifact_id=checkpoint-%zu\n"
                 "owner=11\n"
                 "version=%zu\n"
                 "backing_len=%zu\n"
                 "checksum=%" PRIu64 "\n"
                 "payload_inline=%s\n"
                 "idempotency_key=payload-gc-checkpoint-%zu\n",
                 i + 1U,
                 i + 1U,
                 i + 1U,
                 strlen(payloads[i]),
                 checksums[i],
                 payloads[i],
                 i + 1U);
        status = mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            request,
            response,
            sizeof(response),
            store_path,
            storage_root,
            &limits);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service payload-gc-fixtures: register failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name((uint32_t)status));
            unlink(store_path);
            unlink(journal_path);
            rmdir(blocks_dir);
            rmdir(catalog_dir);
            rmdir(quarantine_dir);
            rmdir(storage_root);
            return 1;
        }
    }
    if (mem_service_make_payload_block_path(storage_root,
                                            checksums[0],
                                            orphan_block,
                                            sizeof(orphan_block)) != 0 ||
        mem_service_make_payload_block_path(storage_root,
                                            checksums[2],
                                            shared_block,
                                            sizeof(shared_block)) != 0 ||
        mem_service_make_payload_block_path(storage_root,
                                            checksums[3],
                                            latest_block,
                                            sizeof(latest_block)) != 0) {
        fprintf(stderr, "mem_service payload-gc-fixtures: block path failed\n");
        return 1;
    }
    if (mem_service_find_record(&svc, "training/gc/checkpoint-1") != NULL ||
        mem_service_find_record(&svc, "training/gc/checkpoint-2") != NULL ||
        mem_service_find_record(&svc, "training/gc/checkpoint-3") == NULL ||
        mem_service_find_record(&svc, "training/gc/checkpoint-4") == NULL ||
        access(orphan_block, F_OK) == 0 ||
        access(shared_block, F_OK) != 0 ||
        access(latest_block, F_OK) != 0) {
        fprintf(stderr, "mem_service payload-gc-fixtures: payload gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        unlink(orphan_block);
        unlink(shared_block);
        unlink(latest_block);
        rmdir(blocks_dir);
        rmdir(catalog_dir);
        rmdir(quarantine_dir);
        rmdir(storage_root);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0 ||
        recovered.record_count != 2U ||
        mem_service_find_record(&recovered, "training/gc/checkpoint-3") == NULL ||
        mem_service_find_record(&recovered, "training/gc/checkpoint-4") == NULL ||
        mem_service_file_contains(journal_path, "training/gc/checkpoint-1") ||
        mem_service_file_contains(journal_path, "training/gc/checkpoint-2")) {
        fprintf(stderr, "mem_service payload-gc-fixtures: durable gc mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        unlink(shared_block);
        unlink(latest_block);
        rmdir(blocks_dir);
        rmdir(catalog_dir);
        rmdir(quarantine_dir);
        rmdir(storage_root);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    unlink(shared_block);
    unlink(latest_block);
    rmdir(blocks_dir);
    rmdir(catalog_dir);
    rmdir(quarantine_dir);
    rmdir(storage_root);
    printf("mem_service payload-gc-fixtures: status=ok "
           "payload_gc=checkpoint-retention-orphan-blocks "
           "payload_blocks_removed=1 shared_block_retained=1 "
           "retained_payload_blocks=2 record_count=%zu durable_reload=1 journal_gc=1\n",
           recovered.record_count);
    return 0;
}

static int mem_service_run_record_retention_kind_fixture_check(void);
static int mem_service_run_record_retention_tenant_fixture_check(void);
static int mem_service_run_record_retention_ttl_fixture_check(void);

int mem_service_run_record_retention_fixture_check(void)
{
    static const char *payloads[5] = {
        "orphan-record-payload",
        "shared-record-payload",
        "shared-record-payload",
        "retained-record-payload-a",
        "retained-record-payload-b",
    };
    struct mem_service svc;
    struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char storage_root[176];
    char store_path[224];
    char journal_path[sizeof(store_path) + 16U];
    char blocks_dir[224];
    char catalog_dir[224];
    char quarantine_dir[224];
    char orphan_block[256];
    char shared_block[256];
    char retained_block_a[256];
    char retained_block_b[256];
    uint64_t checksums[5];
    size_t i;

    snprintf(storage_root,
             sizeof(storage_root),
             "/tmp/linqu_mem_service_record_retention_fixture_%ld",
             (long)getpid());
    if (mem_service_join_path(blocks_dir,
                              sizeof(blocks_dir),
                              storage_root,
                              "blocks") != 0 ||
        mem_service_join_path(catalog_dir,
                              sizeof(catalog_dir),
                              storage_root,
                              "catalog") != 0 ||
        mem_service_join_path(quarantine_dir,
                              sizeof(quarantine_dir),
                              storage_root,
                              "quarantine") != 0 ||
        mem_service_make_catalog_path(storage_root,
                                      "store.snapshot",
                                      store_path,
                                      sizeof(store_path)) != 0 ||
        mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: path setup failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    rmdir(blocks_dir);
    rmdir(catalog_dir);
    rmdir(quarantine_dir);
    rmdir(storage_root);
    if (mem_service_prepare_durable_catalog_layout(storage_root) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: storage setup failed\n");
        return 1;
    }
    memset(&limits, 0, sizeof(limits));
    limits.max_retained_records = 3U;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: init failed\n");
        rmdir(blocks_dir);
        rmdir(catalog_dir);
        rmdir(quarantine_dir);
        rmdir(storage_root);
        return 1;
    }
    for (i = 0U; i < 5U; ++i) {
        char request[384];
        enum mem_service_wire_status status;

        checksums[i] = mem_service_checksum_bytes((const uint8_t *)payloads[i],
                                                  (uint64_t)strlen(payloads[i]));
        snprintf(request,
                 sizeof(request),
                 "key=record-retention-object-%zu\n"
                 "owner=17\n"
                 "version=%zu\n"
                 "backing_len=%zu\n"
                 "checksum=%" PRIu64 "\n"
                 "payload_inline=%s\n"
                 "idempotency_key=record-retention-idem-%zu\n",
                 i + 1U,
                 i + 1U,
                 strlen(payloads[i]),
                 checksums[i],
                 payloads[i],
                 i + 1U);
        status = mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_PUT_OBJECT,
            request,
            response,
            sizeof(response),
            store_path,
            storage_root,
            &limits);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service record-retention-fixtures: put failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name((uint32_t)status));
            unlink(store_path);
            unlink(journal_path);
            rmdir(blocks_dir);
            rmdir(catalog_dir);
            rmdir(quarantine_dir);
            rmdir(storage_root);
            return 1;
        }
    }
    if (mem_service_make_payload_block_path(storage_root,
                                            checksums[0],
                                            orphan_block,
                                            sizeof(orphan_block)) != 0 ||
        mem_service_make_payload_block_path(storage_root,
                                            checksums[2],
                                            shared_block,
                                            sizeof(shared_block)) != 0 ||
        mem_service_make_payload_block_path(storage_root,
                                            checksums[3],
                                            retained_block_a,
                                            sizeof(retained_block_a)) != 0 ||
        mem_service_make_payload_block_path(storage_root,
                                            checksums[4],
                                            retained_block_b,
                                            sizeof(retained_block_b)) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: block path failed\n");
        return 1;
    }
    if (svc.record_count != 3U ||
        mem_service_find_record(&svc, "record-retention-object-1") != NULL ||
        mem_service_find_record(&svc, "record-retention-object-2") != NULL ||
        mem_service_find_record(&svc, "record-retention-object-3") == NULL ||
        mem_service_find_record(&svc, "record-retention-object-4") == NULL ||
        mem_service_find_record(&svc, "record-retention-object-5") == NULL ||
        mem_service_find_idempotency_record(&svc, "record-retention-idem-1") != NULL ||
        mem_service_find_idempotency_record(&svc, "record-retention-idem-2") != NULL ||
        access(orphan_block, F_OK) == 0 ||
        access(shared_block, F_OK) != 0 ||
        access(retained_block_a, F_OK) != 0 ||
        access(retained_block_b, F_OK) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: retention mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        unlink(orphan_block);
        unlink(shared_block);
        unlink(retained_block_a);
        unlink(retained_block_b);
        rmdir(blocks_dir);
        rmdir(catalog_dir);
        rmdir(quarantine_dir);
        rmdir(storage_root);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0 ||
        recovered.record_count != 3U ||
        mem_service_find_record(&recovered, "record-retention-object-1") != NULL ||
        mem_service_find_record(&recovered, "record-retention-object-2") != NULL ||
        mem_service_find_record(&recovered, "record-retention-object-3") == NULL ||
        mem_service_find_record(&recovered, "record-retention-object-4") == NULL ||
        mem_service_find_record(&recovered, "record-retention-object-5") == NULL ||
        mem_service_file_contains(journal_path, "record-retention-object-1") ||
        mem_service_file_contains(journal_path, "record-retention-object-2")) {
        fprintf(stderr, "mem_service record-retention-fixtures: durable mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        unlink(shared_block);
        unlink(retained_block_a);
        unlink(retained_block_b);
        rmdir(blocks_dir);
        rmdir(catalog_dir);
        rmdir(quarantine_dir);
        rmdir(storage_root);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    unlink(shared_block);
    unlink(retained_block_a);
    unlink(retained_block_b);
    rmdir(blocks_dir);
    rmdir(catalog_dir);
    rmdir(quarantine_dir);
    rmdir(storage_root);
    printf("mem_service record-retention-fixtures: status=ok "
           "record_retention=latest max_retained_records=%" PRIu64
           " record_count=%zu pruned_records=2 payload_blocks_removed=1 "
           "shared_block_retained=1 idempotency_gc=1 durable_reload=1 journal_gc=1\n",
           limits.max_retained_records,
           recovered.record_count);
    return mem_service_run_record_retention_kind_fixture_check();
}

static int mem_service_run_record_retention_kind_fixture_check(void)
{
    struct mem_service svc;
    struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[176];
    char journal_path[sizeof(store_path) + 16U];
    size_t i;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_record_retention_kind_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: kind journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    memset(&limits, 0, sizeof(limits));
    limits.max_retained_records = 2U;
    limits.max_retained_record_kind = MEM_SERVICE_RECORD_TRAINING_ARTIFACT;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: kind init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_PUT_OBJECT,
            "key=record-retention-kind-object\n"
            "owner=3\n"
            "version=1\n"
            "backing_len=8\n"
            "checksum=1901\n"
            "idempotency_key=record-retention-kind-object-idem\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits) != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr, "mem_service record-retention-fixtures: kind object put failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    for (i = 0U; i < 4U; ++i) {
        char request[384];
        enum mem_service_wire_status status;

        snprintf(request,
                 sizeof(request),
                 "key=training/kind-retention/artifact-%zu\n"
                 "session_id=kind-retention-session\n"
                 "model_key=kind-retention-model\n"
                 "artifact_kind=gradient\n"
                 "artifact_id=gradient-%zu\n"
                 "owner=9\n"
                 "version=%zu\n"
                 "checksum=%zu\n"
                 "idempotency_key=record-retention-kind-artifact-%zu\n",
                 i + 1U,
                 i + 1U,
                 i + 1U,
                 2100U + i,
                 i + 1U);
        status = mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            request,
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service record-retention-fixtures: kind artifact failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name((uint32_t)status));
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    if (svc.record_count != 3U ||
        mem_service_count_retained_kind_records(
            &svc,
            MEM_SERVICE_RECORD_TRAINING_ARTIFACT,
            false,
            0U) != 2U ||
        mem_service_find_record(&svc, "record-retention-kind-object") == NULL ||
        mem_service_find_record(&svc, "training/kind-retention/artifact-1") != NULL ||
        mem_service_find_record(&svc, "training/kind-retention/artifact-2") != NULL ||
        mem_service_find_record(&svc, "training/kind-retention/artifact-3") == NULL ||
        mem_service_find_record(&svc, "training/kind-retention/artifact-4") == NULL ||
        mem_service_find_idempotency_record(&svc,
                                            "record-retention-kind-artifact-1") != NULL ||
        mem_service_find_idempotency_record(&svc,
                                            "record-retention-kind-artifact-2") != NULL) {
        fprintf(stderr, "mem_service record-retention-fixtures: kind retention mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0 ||
        recovered.record_count != 3U ||
        mem_service_count_retained_kind_records(
            &recovered,
            MEM_SERVICE_RECORD_TRAINING_ARTIFACT,
            false,
            0U) != 2U ||
        mem_service_find_record(&recovered, "record-retention-kind-object") == NULL ||
        mem_service_find_record(&recovered, "training/kind-retention/artifact-1") !=
            NULL ||
        mem_service_find_record(&recovered, "training/kind-retention/artifact-2") !=
            NULL ||
        mem_service_find_record(&recovered, "training/kind-retention/artifact-3") ==
            NULL ||
        mem_service_find_record(&recovered, "training/kind-retention/artifact-4") ==
            NULL ||
        mem_service_file_contains(journal_path,
                                  "training/kind-retention/artifact-1") ||
        mem_service_file_contains(journal_path,
                                  "training/kind-retention/artifact-2")) {
        fprintf(stderr, "mem_service record-retention-fixtures: kind durable mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service record-retention-kind-fixtures: status=ok "
           "record_retention=kind:training-artifact:latest "
           "max_retained_records=%" PRIu64
           " retained_training_artifacts=%" PRIu64
           " non_matching_object_retained=1 pruned_records=2 "
           "idempotency_gc=1 durable_reload=1 journal_gc=1\n",
           limits.max_retained_records,
           mem_service_count_retained_kind_records(
               &recovered,
               MEM_SERVICE_RECORD_TRAINING_ARTIFACT,
               false,
               0U));
    return mem_service_run_record_retention_tenant_fixture_check();
}

static int mem_service_run_record_retention_tenant_fixture_check(void)
{
    static struct mem_service svc;
    static struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[176];
    char journal_path[sizeof(store_path) + 16U];
    size_t i;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_record_retention_tenant_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: tenant journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    memset(&limits, 0, sizeof(limits));
    limits.max_retained_records = 2U;
    limits.max_retained_record_tenant_enabled = true;
    limits.max_retained_record_tenant = 7U;
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: tenant init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_PUT_OBJECT,
            "key=record-retention-tenant-owner3-object\n"
            "owner=3\n"
            "version=1\n"
            "backing_len=8\n"
            "checksum=2301\n"
            "idempotency_key=record-retention-tenant-owner3-idem\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits) != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr, "mem_service record-retention-fixtures: tenant object put failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    for (i = 0U; i < 4U; ++i) {
        char request[384];
        enum mem_service_wire_status status;

        snprintf(request,
                 sizeof(request),
                 "key=training/tenant-retention/owner7-artifact-%zu\n"
                 "session_id=tenant-retention-session\n"
                 "model_key=tenant-retention-model\n"
                 "artifact_kind=gradient\n"
                 "artifact_id=owner7-gradient-%zu\n"
                 "owner=7\n"
                 "version=%zu\n"
                 "checksum=%zu\n"
                 "idempotency_key=record-retention-tenant-owner7-artifact-%zu\n",
                 i + 1U,
                 i + 1U,
                 i + 1U,
                 2400U + i,
                 i + 1U);
        status = mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            request,
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits);
        if (status != MEM_SERVICE_WIRE_STATUS_OK) {
            fprintf(stderr,
                    "mem_service record-retention-fixtures: tenant owner7 artifact failed i=%zu status=%s\n",
                    i,
                    mem_service_wire_status_name((uint32_t)status));
            unlink(store_path);
            unlink(journal_path);
            return 1;
        }
    }
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            "key=training/tenant-retention/owner9-artifact\n"
            "session_id=tenant-retention-session\n"
            "model_key=tenant-retention-model\n"
            "artifact_kind=gradient\n"
            "artifact_id=owner9-gradient\n"
            "owner=9\n"
            "version=9\n"
            "checksum=2909\n"
            "idempotency_key=record-retention-tenant-owner9-artifact\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits) != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: tenant owner9 artifact failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (svc.record_count != 4U ||
        mem_service_count_retained_kind_records(&svc, 0U, true, 7U) != 2U ||
        mem_service_find_record(&svc,
                                "record-retention-tenant-owner3-object") == NULL ||
        mem_service_find_record(&svc,
                                "training/tenant-retention/owner7-artifact-1") !=
            NULL ||
        mem_service_find_record(&svc,
                                "training/tenant-retention/owner7-artifact-2") !=
            NULL ||
        mem_service_find_record(&svc,
                                "training/tenant-retention/owner7-artifact-3") ==
            NULL ||
        mem_service_find_record(&svc,
                                "training/tenant-retention/owner7-artifact-4") ==
            NULL ||
        mem_service_find_record(&svc,
                                "training/tenant-retention/owner9-artifact") == NULL ||
        mem_service_find_idempotency_record(
            &svc,
            "record-retention-tenant-owner7-artifact-1") != NULL ||
        mem_service_find_idempotency_record(
            &svc,
            "record-retention-tenant-owner7-artifact-2") != NULL) {
        fprintf(stderr, "mem_service record-retention-fixtures: tenant mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0 ||
        recovered.record_count != 4U ||
        mem_service_count_retained_kind_records(&recovered, 0U, true, 7U) != 2U ||
        mem_service_find_record(&recovered,
                                "record-retention-tenant-owner3-object") == NULL ||
        mem_service_find_record(&recovered,
                                "training/tenant-retention/owner7-artifact-1") !=
            NULL ||
        mem_service_find_record(&recovered,
                                "training/tenant-retention/owner7-artifact-2") !=
            NULL ||
        mem_service_find_record(&recovered,
                                "training/tenant-retention/owner7-artifact-3") ==
            NULL ||
        mem_service_find_record(&recovered,
                                "training/tenant-retention/owner7-artifact-4") ==
            NULL ||
        mem_service_find_record(&recovered,
                                "training/tenant-retention/owner9-artifact") == NULL ||
        mem_service_file_contains(journal_path,
                                  "training/tenant-retention/owner7-artifact-1") ||
        mem_service_file_contains(journal_path,
                                  "training/tenant-retention/owner7-artifact-2")) {
        fprintf(stderr, "mem_service record-retention-fixtures: tenant durable mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service record-retention-tenant-fixtures: status=ok "
           "record_retention=tenant:7:latest "
           "max_retained_records=%" PRIu64
           " retained_tenant_records=%" PRIu64
           " non_matching_tenant_retained=2 pruned_records=2 "
           "idempotency_gc=1 durable_reload=1 journal_gc=1\n",
           limits.max_retained_records,
           mem_service_count_retained_kind_records(&recovered, 0U, true, 7U));
    return mem_service_run_record_retention_ttl_fixture_check();
}

static int mem_service_run_record_retention_ttl_fixture_check(void)
{
    static struct mem_service svc;
    static struct mem_service recovered;
    struct mem_service_daemon_limits limits;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char store_path[176];
    char journal_path[sizeof(store_path) + 16U];
    struct mem_service_record *old_artifact;
    struct mem_service_record *fresh_artifact;
    uint64_t now_ms;

    snprintf(store_path,
             sizeof(store_path),
             "/tmp/linqu_mem_service_record_retention_ttl_fixture_%ld.store",
             (long)getpid());
    if (mem_service_make_journal_path(store_path,
                                      journal_path,
                                      sizeof(journal_path)) != 0) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: ttl journal path failed\n");
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    memset(&limits, 0, sizeof(limits));
    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service record-retention-fixtures: ttl init failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            "key=training/ttl-retention/old-artifact\n"
            "session_id=ttl-retention-session\n"
            "model_key=ttl-retention-model\n"
            "artifact_kind=gradient\n"
            "artifact_id=old-gradient\n"
            "owner=7\n"
            "version=1\n"
            "checksum=3101\n"
            "idempotency_key=record-retention-ttl-old-artifact\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            NULL) != MEM_SERVICE_WIRE_STATUS_OK ||
        mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
            "key=training/ttl-retention/fresh-artifact\n"
            "session_id=ttl-retention-session\n"
            "model_key=ttl-retention-model\n"
            "artifact_kind=gradient\n"
            "artifact_id=fresh-gradient\n"
            "owner=7\n"
            "version=2\n"
            "checksum=3102\n"
            "idempotency_key=record-retention-ttl-fresh-artifact\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            NULL) != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: ttl artifact setup failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    old_artifact = mem_service_find_record(&svc, "training/ttl-retention/old-artifact");
    fresh_artifact =
        mem_service_find_record(&svc, "training/ttl-retention/fresh-artifact");
    now_ms = mem_service_wall_clock_ms();
    if (old_artifact == NULL || fresh_artifact == NULL || now_ms <= 120000U) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: ttl timestamp setup failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    old_artifact->object_publish_monotonic_ms = now_ms - 120000U;
    fresh_artifact->object_publish_monotonic_ms = now_ms;
    memset(&limits, 0, sizeof(limits));
    limits.max_retained_record_age_ms = 60000U;
    limits.max_retained_record_kind = MEM_SERVICE_RECORD_TRAINING_ARTIFACT;
    if (mem_service_handle_operation_with_limits(
            &svc,
            MEM_SERVICE_WIRE_OP_PUT_OBJECT,
            "key=record-retention-ttl-trigger-object\n"
            "owner=7\n"
            "version=1\n"
            "backing_len=8\n"
            "checksum=3301\n",
            response,
            sizeof(response),
            store_path,
            NULL,
            &limits) != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: ttl trigger failed\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (svc.record_count != 2U ||
        mem_service_find_record(&svc, "training/ttl-retention/old-artifact") !=
            NULL ||
        mem_service_find_record(&svc, "training/ttl-retention/fresh-artifact") ==
            NULL ||
        mem_service_find_record(&svc, "record-retention-ttl-trigger-object") ==
            NULL ||
        mem_service_find_idempotency_record(&svc,
                                            "record-retention-ttl-old-artifact") !=
            NULL ||
        mem_service_find_idempotency_record(&svc,
                                            "record-retention-ttl-fresh-artifact") ==
            NULL) {
        fprintf(stderr, "mem_service record-retention-fixtures: ttl mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    if (mem_service_init(&recovered, true, true, true) != 0 ||
        mem_service_load_durable_store(&recovered, store_path) != 0 ||
        recovered.record_count != 2U ||
        mem_service_find_record(&recovered,
                                "training/ttl-retention/old-artifact") != NULL ||
        mem_service_find_record(&recovered,
                                "training/ttl-retention/fresh-artifact") == NULL ||
        mem_service_find_record(&recovered,
                                "record-retention-ttl-trigger-object") == NULL ||
        mem_service_file_contains(journal_path,
                                  "training/ttl-retention/old-artifact")) {
        fprintf(stderr,
                "mem_service record-retention-fixtures: ttl durable mismatch\n");
        unlink(store_path);
        unlink(journal_path);
        return 1;
    }
    unlink(store_path);
    unlink(journal_path);
    printf("mem_service record-retention-ttl-fixtures: status=ok "
           "record_retention=kind:training-artifact:ttl-ms "
           "max_retained_record_age_ms=%" PRIu64
           " pruned_expired_records=1 fresh_record_retained=1 "
           "non_matching_object_retained=1 idempotency_gc=1 "
           "durable_reload=1 journal_gc=1\n",
           limits.max_retained_record_age_ms);
    return 0;
}

static int mem_service_handle_client(int client_fd,
                                     struct mem_service *svc,
                                     const char *store_path,
                                     const char *storage_root,
                                     const struct mem_service_daemon_limits *limits)
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
    if (mem_service_request_exceeds_payload_limit(request.payload_len, limits)) {
        mem_service_record_operation_metrics(
            svc,
            (enum mem_service_wire_operation)request.operation,
            MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED,
            0);
        return mem_service_send_response(client_fd,
                                         &request,
                                         MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED,
                                         "status=capacity_exceeded\nquota=max_payload_bytes\n");
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
    status = mem_service_handle_operation_with_limits(
        svc,
        (enum mem_service_wire_operation)request.operation,
        (const char *)request_payload,
        response_payload,
        sizeof(response_payload),
        store_path,
        storage_root,
        limits);
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
    if (addr->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
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
    return mem_service_run_unix_daemon_with_store_metrics_catalog_and_limits(
        listen_spec,
        store_path,
        metrics_listen_spec,
        storage_root,
        NULL);
}

int mem_service_run_unix_daemon_with_store_metrics_catalog_and_limits(
    const char *listen_spec,
    const char *store_path,
    const char *metrics_listen_spec,
    const char *storage_root,
    const struct mem_service_daemon_limits *limits)
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
    if (mem_service_admit_or_migrate_catalog_schema_version(storage_root,
                                                            store_path) != 0) {
        fprintf(stderr,
                "mem_service serve: unknown catalog schema version root=%s\n",
                storage_root != NULL ? storage_root : "");
        return 1;
    }
    if (mem_service_load_durable_store(&svc, store_path) != 0) {
        fprintf(stderr, "mem_service serve: store load failed path=%s\n", store_path);
        return 1;
    }
    if (limits != NULL && limits->max_audit_events > 0 &&
        mem_service_apply_audit_retention(&svc, limits->max_audit_events) &&
        store_path != NULL && store_path[0] != '\0' &&
        (mem_service_save_store(&svc, store_path) != 0 ||
         mem_service_compact_journal_now(store_path) != 0)) {
        fprintf(stderr, "mem_service serve: retention save failed path=%s\n", store_path);
        return 1;
    }
    if (limits != NULL && limits->max_checkpoint_records > 0 &&
        mem_service_apply_checkpoint_retention(&svc,
                                               limits->max_checkpoint_records,
                                               storage_root,
                                               NULL) &&
        store_path != NULL && store_path[0] != '\0' &&
        (mem_service_save_store(&svc, store_path) != 0 ||
         mem_service_compact_journal_now(store_path) != 0)) {
        fprintf(stderr,
                "mem_service serve: checkpoint retention save failed path=%s\n",
                store_path);
        return 1;
    }
    if (limits != NULL &&
        (limits->max_retained_records > 0 ||
         limits->max_retained_record_age_ms > 0) &&
        mem_service_apply_record_retention(&svc,
                                           limits->max_retained_records,
                                           limits->max_retained_record_age_ms,
                                           limits->max_retained_record_kind,
                                           limits->max_retained_record_tenant_enabled,
                                           limits->max_retained_record_tenant,
                                           storage_root,
                                           NULL) &&
        store_path != NULL && store_path[0] != '\0' &&
        (mem_service_save_store(&svc, store_path) != 0 ||
         mem_service_compact_journal_now(store_path) != 0)) {
        fprintf(stderr,
                "mem_service serve: record retention save failed path=%s\n",
                store_path);
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
    if (limits != NULL && limits->max_records > 0) {
        printf(" max_records=%" PRIu64, limits->max_records);
    }
    if (limits != NULL && limits->max_payload_bytes > 0) {
        printf(" max_payload_bytes=%" PRIu64, limits->max_payload_bytes);
    }
    if (limits != NULL && limits->max_audit_events > 0) {
        printf(" max_audit_events=%" PRIu64, limits->max_audit_events);
    }
    if (limits != NULL && limits->max_checkpoint_records > 0) {
        printf(" max_checkpoint_records=%" PRIu64, limits->max_checkpoint_records);
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
                                              storage_root,
                                              limits) != 0) {
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
