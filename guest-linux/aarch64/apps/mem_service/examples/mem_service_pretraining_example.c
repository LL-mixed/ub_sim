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

typedef int (*mem_service_training_publish_fn)(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

typedef int (*mem_service_training_resolve_fn)(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

struct mem_service_pretraining_case {
    const char *artifact_kind;
    struct mem_service_client_training_ref ref;
    mem_service_training_publish_fn publish;
    mem_service_training_resolve_fn resolve;
};

static int publish_and_resolve_training_ref(
    const struct mem_service_client *client,
    const struct mem_service_pretraining_case *test_case)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref_query query;

    if (test_case == NULL || test_case->publish == NULL ||
        test_case->resolve == NULL) {
        return fail("invalid training case");
    }
    memset(&query, 0, sizeof(query));
    query.key = test_case->ref.key;
    query.expected_session_id = test_case->ref.session_id;
    query.expected_model_key = test_case->ref.model_key;
    query.expected_artifact_id = test_case->ref.artifact_id;
    query.has_expected_version = test_case->ref.has_version;
    query.expected_version = test_case->ref.version;
    query.has_expected_checksum = test_case->ref.has_checksum;
    query.expected_checksum = test_case->ref.checksum;
    if (expect_ok(test_case->publish(client,
                                     &test_case->ref,
                                     &record,
                                     &status),
                  status,
                  "publish_training_ref") != 0 ||
        expect_ok(test_case->resolve(client,
                                     &query,
                                     &record,
                                     &status),
                  status,
                  "resolve_training_ref") != 0) {
        return 1;
    }
    if (strcmp(record.key, test_case->ref.key) != 0 ||
        strcmp(record.artifact_kind, test_case->artifact_kind) != 0 ||
        record.version != test_case->ref.version ||
        record.object_payload_checksum != test_case->ref.checksum) {
        return fail("training ref mismatch");
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct mem_service_client client;
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    const struct mem_service_pretraining_case artifacts[] = {
        {
            .artifact_kind = "dataset-shard",
            .ref = {
                .key = "training/run-a/dataset-shard-0000",
                .idempotency_key = "training/run-a/dataset-shard-0000/v1",
                .session_id = "run-a",
                .request_id = "global-step-0",
                .model_key = "qwen3-14b-pretrain",
                .artifact_id = "dataset-shard-0000",
                .has_owner = true,
                .owner = 0,
                .has_payload_kind = true,
                .payload_kind = 10,
                .has_backing_offset = true,
                .backing_offset = 4096,
                .has_backing_len = true,
                .backing_len = 1048576,
                .has_checksum = true,
                .checksum = 0x01020304,
                .has_version = true,
                .version = 1,
            },
            .publish = mem_service_client_publish_dataset_shard,
            .resolve = mem_service_client_resolve_dataset_shard,
        },
        {
            .artifact_kind = "sample-batch",
            .ref = {
                .key = "training/run-a/sample-batch-0000",
                .idempotency_key = "training/run-a/sample-batch-0000/v2",
                .session_id = "run-a",
                .request_id = "global-step-0",
                .model_key = "qwen3-14b-pretrain",
                .artifact_id = "sample-batch-0000",
                .has_owner = true,
                .owner = 1,
                .has_payload_kind = true,
                .payload_kind = 11,
                .has_backing_offset = true,
                .backing_offset = 2097152,
                .has_backing_len = true,
                .backing_len = 524288,
                .has_checksum = true,
                .checksum = 0x05060708,
                .has_version = true,
                .version = 2,
            },
            .publish = mem_service_client_publish_sample_batch,
            .resolve = mem_service_client_resolve_sample_batch,
        },
        {
            .artifact_kind = "checkpoint",
            .ref = {
                .key = "training/run-a/checkpoint-0001",
                .idempotency_key = "training/run-a/checkpoint-0001/v3",
                .session_id = "run-a",
                .request_id = "global-step-1",
                .model_key = "qwen3-14b-pretrain",
                .artifact_id = "checkpoint-0001",
                .has_owner = true,
                .owner = 2,
                .has_payload_kind = true,
                .payload_kind = 12,
                .has_backing_offset = true,
                .backing_offset = 4194304,
                .has_backing_len = true,
                .backing_len = 8388608,
                .has_checksum = true,
                .checksum = 0x090a0b0c,
                .has_version = true,
                .version = 3,
            },
            .publish = mem_service_client_publish_checkpoint,
            .resolve = mem_service_client_resolve_checkpoint,
        },
        {
            .artifact_kind = "gradient-bucket",
            .ref = {
                .key = "training/run-a/gradient-bucket-0001",
                .idempotency_key = "training/run-a/gradient-bucket-0001/v4",
                .session_id = "run-a",
                .request_id = "global-step-1",
                .model_key = "qwen3-14b-pretrain",
                .artifact_id = "gradient-bucket-0001",
                .has_owner = true,
                .owner = 3,
                .has_payload_kind = true,
                .payload_kind = 13,
                .has_backing_offset = true,
                .backing_offset = 12582912,
                .has_backing_len = true,
                .backing_len = 2097152,
                .has_checksum = true,
                .checksum = 0x0d0e0f10,
                .has_version = true,
                .version = 4,
            },
            .publish = mem_service_client_publish_gradient_bucket,
            .resolve = mem_service_client_resolve_gradient_bucket,
        },
        {
            .artifact_kind = "optimizer-state",
            .ref = {
                .key = "training/run-a/optimizer-state-0001",
                .idempotency_key = "training/run-a/optimizer-state-0001/v5",
                .session_id = "run-a",
                .request_id = "global-step-1",
                .model_key = "qwen3-14b-pretrain",
                .artifact_id = "optimizer-state-0001",
                .has_owner = true,
                .owner = 4,
                .has_payload_kind = true,
                .payload_kind = 14,
                .has_backing_offset = true,
                .backing_offset = 14680064,
                .has_backing_len = true,
                .backing_len = 4194304,
                .has_checksum = true,
                .checksum = 0x11121314,
                .has_version = true,
                .version = 5,
            },
            .publish = mem_service_client_publish_optimizer_state,
            .resolve = mem_service_client_resolve_optimizer_state,
        },
        {
            .artifact_kind = MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND,
            .ref = {
                .key = "training/run-a/global-step-0001/commit",
                .idempotency_key = "training/run-a/global-step-0001/commit/v6",
                .session_id = "run-a",
                .request_id = "global-step-1",
                .model_key = "qwen3-14b-pretrain",
                .artifact_id = "global-step-0001",
                .has_owner = true,
                .owner = 0,
                .has_payload_kind = true,
                .payload_kind = 15,
                .has_backing_offset = true,
                .backing_offset = 18874368,
                .has_backing_len = true,
                .backing_len = 64,
                .has_checksum = true,
                .checksum = 0x15161718,
                .has_version = true,
                .version = 6,
            },
            .publish = mem_service_client_commit_training_step,
            .resolve = mem_service_client_resolve_training_step,
        },
    };
    size_t i;

    if (argc != 2) {
        return fail("usage: mem_service_pretraining_example unix:/path/to.sock");
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
                  "ready") != 0) {
        return 1;
    }
    for (i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); ++i) {
        if (publish_and_resolve_training_ref(&client, &artifacts[i]) != 0) {
            return 1;
        }
    }

    printf("mem_service_pretraining_example=ok artifacts=%zu last_kind=%s "
           "last_version=%llu\n",
           sizeof(artifacts) / sizeof(artifacts[0]),
           artifacts[(sizeof(artifacts) / sizeof(artifacts[0])) - 1].artifact_kind,
           (unsigned long long)artifacts[(sizeof(artifacts) /
                                          sizeof(artifacts[0])) - 1]
               .ref.version);
    return 0;
}
