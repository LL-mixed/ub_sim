#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_wire_client.h"

typedef int (*pretraining_publish_fn)(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

typedef int (*pretraining_resolve_fn)(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

struct pretraining_case {
    const char *artifact_kind;
    struct mem_service_client_training_ref ref;
    pretraining_publish_fn publish;
    pretraining_resolve_fn resolve;
};

static const struct pretraining_case pretraining_cases[] = {
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

static int expect_not_found(int rc,
                            enum mem_service_wire_status status,
                            const char *operation)
{
    if (status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
        return 0;
    }
    fprintf(stderr,
            "%s expected not found rc=%d status=%u\n",
            operation,
            rc,
            (unsigned)status);
    return 1;
}

static int connect_client(struct mem_service_client *client,
                          const char *connect_spec)
{
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;

    mem_service_wire_client_options_init(&options);
    options.timeout_ms = 2000;
    options.max_attempts = 3;
    options.retry_backoff_ms = 10;
    options.retry_on_timeout = 1;
    mem_service_client_init_with_options(client, connect_spec, &options);

    if (expect_ok(mem_service_client_health(client, &status),
                  status,
                  "health") != 0 ||
        expect_ok(mem_service_client_ready(client, &status),
                  status,
                  "ready") != 0) {
        return 1;
    }
    return 0;
}

static void build_query(const struct mem_service_client_training_ref *ref,
                        struct mem_service_client_training_ref_query *query)
{
    memset(query, 0, sizeof(*query));
    query->key = ref->key;
    query->expected_session_id = ref->session_id;
    query->expected_model_key = ref->model_key;
    query->expected_artifact_id = ref->artifact_id;
    query->has_expected_owner = ref->has_owner;
    query->expected_owner = ref->owner;
    query->has_expected_version = ref->has_version;
    query->expected_version = ref->version;
    query->has_expected_checksum = ref->has_checksum;
    query->expected_checksum = ref->checksum;
}

static int validate_record(const struct pretraining_case *test_case,
                           const struct mem_service_client_record *record)
{
    if (strcmp(record->key, test_case->ref.key) != 0 ||
        strcmp(record->session_id, test_case->ref.session_id) != 0 ||
        strcmp(record->model_key, test_case->ref.model_key) != 0 ||
        strcmp(record->artifact_kind, test_case->artifact_kind) != 0 ||
        strcmp(record->artifact_id, test_case->ref.artifact_id) != 0 ||
        record->object_owner_node != test_case->ref.owner ||
        record->object_payload_kind != test_case->ref.payload_kind ||
        record->object_backing_offset != test_case->ref.backing_offset ||
        record->object_backing_len != test_case->ref.backing_len ||
        record->object_payload_checksum != test_case->ref.checksum ||
        record->version != test_case->ref.version) {
        fprintf(stderr,
                "pretraining record mismatch key=%s kind=%s version=%llu\n",
                record->key,
                record->artifact_kind,
                (unsigned long long)record->version);
        return 1;
    }
    return 0;
}

static int resolve_case(const struct mem_service_client *client,
                        const struct pretraining_case *test_case,
                        struct mem_service_client_record *record)
{
    struct mem_service_client_training_ref_query query;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;

    build_query(&test_case->ref, &query);
    if (expect_ok(test_case->resolve(client, &query, record, &status),
                  status,
                  "resolve_pretraining_ref") != 0) {
        return 1;
    }
    return validate_record(test_case, record);
}

static int run_publish_mode(const char *connect_spec)
{
    struct mem_service_client client;
    struct mem_service_client_record record;
    struct mem_service_client_training_ref_query query;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    size_t i;

    if (connect_client(&client, connect_spec) != 0) {
        return 1;
    }

    build_query(&pretraining_cases[0].ref, &query);
    if (expect_not_found(pretraining_cases[0].resolve(&client,
                                                      &query,
                                                      &record,
                                                      &status),
                         status,
                         "pretraining_preflight_resolve") != 0) {
        return 1;
    }

    for (i = 0; i < sizeof(pretraining_cases) / sizeof(pretraining_cases[0]); ++i) {
        if (expect_ok(pretraining_cases[i].publish(&client,
                                                   &pretraining_cases[i].ref,
                                                   &record,
                                                   &status),
                      status,
                      "publish_pretraining_ref") != 0 ||
            validate_record(&pretraining_cases[i], &record) != 0) {
            return 1;
        }
    }

    printf("linqu_pretraining_client_mem_service_publish=ok connect=%s "
           "model_key=%s artifacts=%zu first=%s last=%s last_version=%llu\n",
           connect_spec,
           pretraining_cases[0].ref.model_key,
           sizeof(pretraining_cases) / sizeof(pretraining_cases[0]),
           pretraining_cases[0].ref.key,
           pretraining_cases[(sizeof(pretraining_cases) /
                              sizeof(pretraining_cases[0])) - 1]
               .ref.key,
           (unsigned long long)pretraining_cases[(sizeof(pretraining_cases) /
                                                  sizeof(pretraining_cases[0])) - 1]
               .ref.version);
    return 0;
}

static int run_verify_mode(const char *connect_spec)
{
    struct mem_service_client client;
    struct mem_service_client_record record;
    size_t i;

    if (connect_client(&client, connect_spec) != 0) {
        return 1;
    }

    for (i = 0; i < sizeof(pretraining_cases) / sizeof(pretraining_cases[0]); ++i) {
        if (resolve_case(&client, &pretraining_cases[i], &record) != 0) {
            return 1;
        }
    }

    printf("linqu_pretraining_client_mem_service_verify=ok connect=%s "
           "warm_reuse=1 model_key=%s artifacts=%zu first=%s last=%s "
           "last_version=%llu\n",
           connect_spec,
           pretraining_cases[0].ref.model_key,
           sizeof(pretraining_cases) / sizeof(pretraining_cases[0]),
           pretraining_cases[0].ref.key,
           pretraining_cases[(sizeof(pretraining_cases) /
                              sizeof(pretraining_cases[0])) - 1]
               .ref.key,
           (unsigned long long)pretraining_cases[(sizeof(pretraining_cases) /
                                                  sizeof(pretraining_cases[0])) - 1]
               .ref.version);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        return fail("usage: linqu_pretraining_client "
                    "--mem-service-pretraining-publish|"
                    "--mem-service-pretraining-verify unix:/path/to.sock");
    }

    if (strcmp(argv[1], "--mem-service-pretraining-publish") == 0) {
        return run_publish_mode(argv[2]);
    }
    if (strcmp(argv[1], "--mem-service-pretraining-verify") == 0) {
        return run_verify_mode(argv[2]);
    }

    return fail("unknown linqu_pretraining_client mode");
}
