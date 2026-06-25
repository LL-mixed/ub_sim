#include <stdio.h>
#include <string.h>

#include "mem_service_client.h"

static int fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    return 1;
}

static int expect_ok(int rc,
                     enum mem_service_wire_status status,
                     const char *operation)
{
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "%s failed rc=%d status=%u\n",
                operation,
                rc,
                (unsigned)status);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct mem_service_client client;
    struct mem_service_wire_client_options options;
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_block_entry prefix = {
        .request_id = "serving-request-0",
        .prefix_group = "serving-prefix-qwen3",
        .group_id = "serving-group-0",
        .block_hash = "serving-prefix-block-0",
        .idempotency_key = "serving-prefix-block-0/v1",
        .has_placement_node = true,
        .placement_node = 1,
        .has_placement_level = true,
        .placement_level = 2,
        .has_hot_segment_id = true,
        .hot_segment_id = 4096,
        .state = "filled",
        .has_result_segment_id = true,
        .result_segment_id = 8192,
    };
    struct mem_service_client_block_entry kv = {
        .request_id = "serving-request-0",
        .prefix_group = "serving-prefix-qwen3",
        .group_id = "serving-group-0",
        .block_hash = "serving-kv-block-0",
        .idempotency_key = "serving-kv-block-0/v1",
        .has_placement_node = true,
        .placement_node = 1,
        .has_placement_level = true,
        .placement_level = 3,
        .has_hot_segment_id = true,
        .hot_segment_id = 12288,
        .state = "filled",
        .has_result_segment_id = true,
        .result_segment_id = 16384,
    };
    struct mem_service_client_kv_selector kv_selector = {
        .block_hash = "serving-kv-block-0",
    };
    struct mem_service_client_artifact runtime_handoff = {
        .key = "runtime/serving-session/range-0",
        .idempotency_key = "runtime/serving-session/range-0/v7",
        .session_id = "serving-session",
        .request_id = "serving-request-0",
        .model_key = "qwen3-14b",
        .artifact_kind = "hidden-range",
        .artifact_id = "range-0",
        .has_owner = true,
        .owner = 1,
        .has_payload_kind = true,
        .payload_kind = 2,
        .has_backing_offset = true,
        .backing_offset = 32768,
        .has_backing_len = true,
        .backing_len = 65536,
        .has_checksum = true,
        .checksum = 0x11112222,
        .has_version = true,
        .version = 7,
    };
    struct mem_service_client_artifact_query runtime_query = {
        .key = "runtime/serving-session/range-0",
        .expected_session_id = "serving-session",
        .expected_model_key = "qwen3-14b",
        .expected_artifact_kind = "hidden-range",
        .expected_artifact_id = "range-0",
        .has_expected_version = true,
        .expected_version = 7,
        .has_expected_checksum = true,
        .expected_checksum = 0x11112222,
    };
    struct mem_service_client_artifact logits = {
        .key = "execution/serving-session/logits-0",
        .idempotency_key = "execution/serving-session/logits-0/v8",
        .session_id = "serving-session",
        .request_id = "serving-request-0",
        .model_key = "qwen3-14b",
        .artifact_kind = "logits",
        .artifact_id = "logits-0",
        .has_payload_kind = true,
        .payload_kind = 3,
        .has_checksum = true,
        .checksum = 0x33334444,
        .has_version = true,
        .version = 8,
    };
    struct mem_service_client_artifact_query logits_query = {
        .key = "execution/serving-session/logits-0",
        .expected_session_id = "serving-session",
        .expected_model_key = "qwen3-14b",
        .expected_artifact_kind = "logits",
        .expected_artifact_id = "logits-0",
        .has_expected_version = true,
        .expected_version = 8,
        .has_expected_checksum = true,
        .expected_checksum = 0x33334444,
    };

    if (argc != 2) {
        return fail("usage: mem_service_serving_example unix:/path/to.sock");
    }

    mem_service_wire_client_options_init(&options);
    options.timeout_ms = 2000;
    options.max_attempts = 3;
    options.retry_backoff_ms = 10;
    options.retry_on_timeout = 1;
    mem_service_client_init_with_options(&client, argv[1], &options);
    if (expect_ok(mem_service_client_health(&client, &status),
                  status,
                  "health") != 0 ||
        expect_ok(mem_service_client_ready(&client, &status),
                  status,
                  "ready") != 0 ||
        expect_ok(mem_service_client_register_prefix_entry(&client,
                                                           &prefix,
                                                           &record,
                                                           &status),
                  status,
                  "register_prefix") != 0) {
        return 1;
    }
    if (strcmp(record.block_hash, "serving-prefix-block-0") != 0) {
        return fail("prefix block mismatch");
    }
    if (expect_ok(mem_service_client_lookup_prefix_entry(&client,
                                                         "serving-request-0",
                                                         "serving-prefix-qwen3",
                                                         &record,
                                                         &status),
                  status,
                  "lookup_prefix") != 0 ||
        strcmp(record.state, "filled") != 0) {
        return fail("prefix lookup mismatch");
    }
    if (expect_ok(mem_service_client_publish_kv_segment(&client,
                                                        &kv,
                                                        &record,
                                                        &status),
                  status,
                  "publish_kv") != 0 ||
        expect_ok(mem_service_client_resolve_kv_segment(&client,
                                                        &kv_selector,
                                                        &record,
                                                        &status),
                  status,
                  "resolve_kv") != 0 ||
        strcmp(record.block_hash, "serving-kv-block-0") != 0) {
        return fail("kv resolve mismatch");
    }
    if (expect_ok(mem_service_client_publish_runtime_handoff(&client,
                                                             &runtime_handoff,
                                                             &record,
                                                             &status),
                  status,
                  "publish_runtime_handoff") != 0 ||
        expect_ok(mem_service_client_resolve_runtime_handoff(&client,
                                                             &runtime_query,
                                                             &record,
                                                             &status),
                  status,
                  "resolve_runtime_handoff") != 0 ||
        record.version != 7 ||
        record.object_payload_checksum != 0x11112222) {
        return fail("runtime handoff mismatch");
    }
    if (expect_ok(mem_service_client_register_execution_artifact(&client,
                                                                 &logits,
                                                                 &record,
                                                                 &status),
                  status,
                  "register_execution_artifact") != 0 ||
        expect_ok(mem_service_client_query_execution_artifact(&client,
                                                              &logits_query,
                                                              &record,
                                                              &status),
                  status,
                  "query_execution_artifact") != 0 ||
        record.version != 8 ||
        record.object_payload_checksum != 0x33334444) {
        return fail("execution artifact mismatch");
    }

    printf("mem_service_serving_example=ok prefix=%s kv=%s runtime_version=%llu "
           "logits_version=%llu\n",
           prefix.prefix_group,
           kv.block_hash,
           (unsigned long long)runtime_query.expected_version,
           (unsigned long long)logits_query.expected_version);
    return 0;
}
