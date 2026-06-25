#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "components/mem_service/mem_service_core.h"
#include "components/mem_service/mem_service_daemon.h"
#include "components/mem_service/mem_service_wire_client.h"
#include "components/mem_service/mem_service_wire_payload.h"
#include "components/mem_service/mem_service_wire_schema.h"

#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
#include "components/llm_infer/llm_infer.h"
#endif

#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION 1U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN 8516U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM 0x560b762fU
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT 22U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT 100U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT 1U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT 2U
#define MEM_SERVICE_CONFIG_SCHEMA_VERSION 1U
#define MEM_SERVICE_CLI_STORE_MAGIC "mem_service_store_v1"

static void usage(const char *argv0)
{
    printf("Usage: %s [--smoke] [--self-test]", argv0);
    printf(" [wire-fixtures] [wire-schema] [wire-schema-fixtures]");
    printf(" [store-fixtures] [config-fixtures] [metrics-export-fixtures]");
    printf(" [client-retry-fixtures]");
    printf(" [release-manifest] [release-fixtures]");
    printf(" [serve [--config <path>] [--listen unix:%s] [--store <path>]]",
           MEM_SERVICE_DEFAULT_UNIX_SOCKET);
    printf(" [health|ready|status|list-records|metrics|metrics-export|export-snapshot|export-snapshot-page|export-snapshot-to|restore-snapshot [--connect unix:%s] [--timeout-ms <ms>] [--max-attempts <n>] [--retry-backoff-ms <ms>] [--retry-timeouts]]",
           MEM_SERVICE_DEFAULT_UNIX_SOCKET);
    printf(" [metrics-export accepts --format prometheus-text]");
    printf(" [put-object|get-object|inspect-object|register-prefix|lookup-prefix|publish-kv|resolve-kv]");
    printf(" [publish-runtime-handoff|resolve-runtime-handoff]");
    printf(" [register-execution-artifact|query-execution-artifact]");
    printf(" [register-training-artifact|query-training-artifact]");
    printf(" [mutating commands accept --idempotency-key <key>]");
#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
    printf(" [--inspect-qwen3]");
#endif
    printf("\n");
}

static const char *wire_payload_format_name(uint32_t payload_format)
{
    if (payload_format == MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV) {
        return "text-kv";
    }
    return "unknown";
}

static const char *wire_field_type_name(enum mem_service_wire_payload_field_type type)
{
    if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING) {
        return "string";
    }
    if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32) {
        return "u32";
    }
    if (type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
        return "u64";
    }
    return "unknown";
}

static int append_wire_schema_line(char *manifest,
                                   size_t manifest_len,
                                   size_t *used,
                                   const char *fmt,
                                   ...)
{
    va_list ap;
    int written;

    if (manifest == NULL || used == NULL || *used >= manifest_len) {
        return -1;
    }
    va_start(ap, fmt);
    written = vsnprintf(manifest + *used, manifest_len - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= manifest_len - *used) {
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static size_t wire_schema_operation_count(void)
{
    return sizeof(mem_service_wire_operation_schemas) /
           sizeof(mem_service_wire_operation_schemas[0]);
}

static void wire_schema_count_fields(size_t *field_count_out,
                                     size_t *oneof_count_out,
                                     size_t *oneof_field_count_out)
{
    size_t op_index;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    for (op_index = 0; op_index < wire_schema_operation_count(); ++op_index) {
        const struct mem_service_wire_operation_schema *schema =
            &mem_service_wire_operation_schemas[op_index];
        size_t oneof_index;

        field_count += schema->field_count;
        oneof_count += schema->oneof_count;
        for (oneof_index = 0; oneof_index < schema->oneof_count; ++oneof_index) {
            oneof_field_count += schema->oneofs[oneof_index].field_count;
        }
    }
    if (field_count_out != NULL) {
        *field_count_out = field_count;
    }
    if (oneof_count_out != NULL) {
        *oneof_count_out = oneof_count;
    }
    if (oneof_field_count_out != NULL) {
        *oneof_field_count_out = oneof_field_count;
    }
}

static int render_wire_schema_manifest(char *manifest,
                                       size_t manifest_len,
                                       size_t *used_out)
{
    size_t used = 0;
    size_t op_index;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "mem_service_wire_schema_manifest_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_payload_format=%s\n",
                                wire_payload_format_name(
                                    MEM_SERVICE_WIRE_SCHEMA_FORMAT_TEXT_KV)) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "operation_count=%zu\n",
                                wire_schema_operation_count()) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "field_count=%zu\n",
                                field_count) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "oneof_count=%zu\n",
                                oneof_count) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "oneof_field_count=%zu\n",
                                oneof_field_count) != 0) {
        return -1;
    }
    for (op_index = 0; op_index < wire_schema_operation_count(); ++op_index) {
        const struct mem_service_wire_operation_schema *schema =
            &mem_service_wire_operation_schemas[op_index];
        size_t field_index;
        size_t oneof_index;

        if (append_wire_schema_line(manifest,
                                    manifest_len,
                                    &used,
                                    "operation=%s:%u schema_version=%u "
                                    "payload_format=%s fields=%zu oneofs=%zu\n",
                                    schema->name,
                                    (uint32_t)schema->operation,
                                    schema->schema_version,
                                    wire_payload_format_name(schema->payload_format),
                                    schema->field_count,
                                    schema->oneof_count) != 0) {
            return -1;
        }
        for (field_index = 0; field_index < schema->field_count; ++field_index) {
            const struct mem_service_wire_payload_field *field =
                &schema->fields[field_index];

            if (append_wire_schema_line(manifest,
                                        manifest_len,
                                        &used,
                                        "field=%s.%s type=%s required=%u\n",
                                        schema->name,
                                        field->name,
                                        wire_field_type_name(field->type),
                                        field->required ? 1U : 0U) != 0) {
                return -1;
            }
        }
        for (oneof_index = 0; oneof_index < schema->oneof_count; ++oneof_index) {
            const struct mem_service_wire_payload_oneof *oneof =
                &schema->oneofs[oneof_index];
            size_t oneof_field_index;

            if (append_wire_schema_line(manifest,
                                        manifest_len,
                                        &used,
                                        "oneof=%s.%zu field_count=%zu\n",
                                        schema->name,
                                        oneof_index,
                                        oneof->field_count) != 0) {
                return -1;
            }
            for (oneof_field_index = 0; oneof_field_index < oneof->field_count;
                 ++oneof_field_index) {
                if (append_wire_schema_line(manifest,
                                            manifest_len,
                                            &used,
                                            "oneof_field=%s.%zu.%s\n",
                                            schema->name,
                                            oneof_index,
                                            oneof->field_names[oneof_field_index]) != 0) {
                    return -1;
                }
            }
        }
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_wire_schema_manifest(void)
{
    char manifest[16384];
    size_t used = 0;

    if (render_wire_schema_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service wire-schema: render failed\n");
        return 1;
    }
    (void)used;
    fputs(manifest, stdout);
    return 0;
}

static int run_wire_schema_fixture_check(void)
{
    char manifest[16384];
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_wire_schema_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service wire-schema-fixtures: render failed\n");
        return 1;
    }
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    checksum = mem_service_wire_checksum(manifest, used);
    if (wire_schema_operation_count() !=
        MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: operation count mismatch\n");
        failures -= 1;
    }
    if (field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: field count mismatch\n");
        failures -= 1;
    }
    if (oneof_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: oneof count mismatch\n");
        failures -= 1;
    }
    if (oneof_field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT) {
        fprintf(stderr, "mem_service wire-schema-fixtures: oneof field count mismatch\n");
        failures -= 1;
    }
    if (used != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service wire-schema-fixtures: manifest len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service wire-schema-fixtures: manifest checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service wire-schema-fixtures: status=ok manifest_len=%u "
           "manifest_checksum=0x%08x operations=%u fields=%u oneofs=%u "
           "oneof_fields=%u\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT);
    return 0;
}

static int run_smoke(void)
{
    struct mem_service svc;
    struct mem_service_block_ctx ctx;
    struct mem_service_block_ctx aux_ctx;
    struct mem_service_record block;
    struct mem_service_record aux_block;
    struct mem_service_record prefix;
    struct mem_service_record aux_prefix;
    struct mem_service_record group;
    char block_key[96];

    memset(&svc, 0, sizeof(svc));
    memset(&ctx, 0, sizeof(ctx));
    memset(&aux_ctx, 0, sizeof(aux_ctx));
    memset(&block, 0, sizeof(block));
    memset(&aux_block, 0, sizeof(aux_block));
    memset(&prefix, 0, sizeof(prefix));
    memset(&aux_prefix, 0, sizeof(aux_prefix));
    memset(&group, 0, sizeof(group));

    snprintf(ctx.request_id, sizeof(ctx.request_id), "cli-smoke-request");
    snprintf(ctx.prefix_group, sizeof(ctx.prefix_group), "cli-prefix");
    snprintf(ctx.group_id, sizeof(ctx.group_id), "cli-group");
    snprintf(ctx.block_hash, sizeof(ctx.block_hash), "cli-block-hash");
    ctx.placement_node = 1;
    ctx.placement_level = 2;
    ctx.hot_segment_id = 0x1000;
    ctx.result_segment_id = 0x2000;
    aux_ctx = ctx;
    snprintf(aux_ctx.prefix_group, sizeof(aux_ctx.prefix_group), "cli-prefix-aux");
    snprintf(aux_ctx.block_hash, sizeof(aux_ctx.block_hash), "cli-block-hash-aux");
    aux_ctx.hot_segment_id = 0x3000;
    aux_ctx.result_segment_id = 0x4000;

    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service smoke: init failed\n");
        return 1;
    }
    if (mem_service_bootstrap_kvcache(&svc, &ctx, &block) != 0) {
        fprintf(stderr, "mem_service smoke: bootstrap failed\n");
        return 1;
    }
    if (mem_service_bootstrap_kvcache(&svc, &aux_ctx, &aux_block) != 0) {
        fprintf(stderr, "mem_service smoke: aux bootstrap failed\n");
        return 1;
    }
    if (mem_service_apply_block_result(&svc,
                                       &ctx,
                                       ctx.result_segment_id + 0x10,
                                       MEM_SERVICE_KVCACHE_STATE_RELOADED,
                                       &block) != 0) {
        fprintf(stderr, "mem_service smoke: apply block result failed\n");
        return 1;
    }
    if (mem_service_apply_block_result(&svc,
                                       &aux_ctx,
                                       aux_ctx.result_segment_id + 0x10,
                                       MEM_SERVICE_KVCACHE_STATE_RELOADED,
                                       &aux_block) != 0) {
        fprintf(stderr, "mem_service smoke: apply aux block result failed\n");
        return 1;
    }
    if (mem_service_update_prefix_metadata(&svc, &ctx, &block, &prefix) != 0) {
        fprintf(stderr, "mem_service smoke: prefix metadata update failed\n");
        return 1;
    }
    if (mem_service_update_prefix_metadata(&svc, &aux_ctx, &aux_block, &aux_prefix) != 0) {
        fprintf(stderr, "mem_service smoke: aux prefix metadata update failed\n");
        return 1;
    }
    if (mem_service_get_prefix_group_metadata(&svc, &ctx, &group) != 0) {
        fprintf(stderr, "mem_service smoke: prefix group metadata failed\n");
        return 1;
    }
    mem_service_build_block_key_from_hash(ctx.block_hash, block_key, sizeof(block_key));
    if (mem_service_get_record(&svc, block_key, &block) != 0) {
        fprintf(stderr, "mem_service smoke: block lookup failed\n");
        return 1;
    }
    if (!mem_service_prefix_matches_block_meta(&prefix, &block) ||
        !mem_service_prefix_matches_block_meta(&aux_prefix, &aux_block) ||
        !mem_service_group_covers_blocks(&group, &block, &aux_block)) {
        fprintf(stderr, "mem_service smoke: prefix/group relation failed\n");
        return 1;
    }

    printf("mem_service smoke: status=ok records=%zu block_key=%s state=%s group_members=%u\n",
           svc.record_count,
           block_key,
           mem_service_kvcache_state_name(block.state),
           group.member_count);
    return 0;
}

#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
static int inspect_qwen3(void)
{
    uint32_t node;
    uint32_t nodes = (uint32_t)llm_infer_qwen3_pipeline_nodes();

    printf("mem_service qwen3: model_key=%s nodes=%u layers=%" PRIu64
           " hidden_range_bytes=%" PRIu64 " decode_hidden_bytes=%" PRIu64 "\n",
           llm_infer_qwen3_model_key(),
           nodes,
           llm_infer_qwen3_total_layers(),
           llm_infer_qwen3_hidden_range_bytes(),
           llm_infer_qwen3_decode_hidden_bytes());
    for (node = 0; node < nodes; ++node) {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t next = 0;

        if (llm_infer_qwen3_layer_range_for_node(node, nodes, &start, &end, &next) != 0) {
            fprintf(stderr, "mem_service qwen3: invalid placement node=%u\n", node);
            return 1;
        }
        printf("mem_service qwen3: node=%u layers=[%u,%u) next=%u kv_bytes_per_token=%" PRIu64 "\n",
               node + 1,
               start,
               end,
               next + 1,
               llm_infer_qwen3_range_kv_state_bytes(start, end));
    }
    return 0;
}
#endif

static int run_release_manifest(void)
{
    printf("mem_service_release_manifest_version=1\n");
    printf("service_name=linqu_mem_service\n");
    printf("core_binary=bin/linqu_mem_service\n");
    printf("qwen3_adapter_binary_optional=bin/linqu_mem_service_qwen3\n");
    printf("default_endpoint=%s\n", mem_service_default_unix_socket_spec());
    printf("wire_version=%u\n", MEM_SERVICE_WIRE_VERSION);
    printf("wire_header_len=%u\n", MEM_SERVICE_WIRE_HEADER_LEN);
    printf("wire_schema_version=%u\n", MEM_SERVICE_WIRE_SCHEMA_VERSION);
    printf("wire_payload_format=text-kv\n");
    printf("wire_schema_manifest=share/lingqu/mem_service/wire-schema.txt\n");
    printf("wire_schema_manifest_len=%u\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN);
    printf("wire_schema_manifest_checksum=0x%08x\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM);
    printf("config_schema_version=%u\n", MEM_SERVICE_CONFIG_SCHEMA_VERSION);
    printf("config_schema=share/lingqu/mem_service/config/mem_service.conf.schema\n");
    printf("config_example=share/lingqu/mem_service/config/mem_service.example.conf\n");
    printf("deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service\n");
    printf("metrics_export_format=prometheus-text\n");
    printf("client_retry_policy=explicit-max-attempts-backoff\n");
    printf("client_api=pretraining-refs-v1\n");
    printf("public_header=include/lingqu/mem_service/mem_service.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_core.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_client.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire_client.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire_payload.h\n");
    printf("public_header=include/lingqu/mem_service/mem_service_wire_schema.h\n");
    printf("public_header=include/lingqu/mem_service/lingqu_object_service.h\n");
    printf("client_source=src/lingqu/mem_service/mem_service_client.c\n");
    printf("client_source=src/lingqu/mem_service/mem_service_wire_client.c\n");
    printf("example_source=share/lingqu/mem_service/examples/mem_service_serving_example.c\n");
    printf("example_source=share/lingqu/mem_service/examples/mem_service_pretraining_example.c\n");
    printf("operation=health:%u\n", MEM_SERVICE_WIRE_OP_HEALTH);
    printf("operation=ready:%u\n", MEM_SERVICE_WIRE_OP_READY);
    printf("operation=status:%u\n", MEM_SERVICE_WIRE_OP_STATUS);
    printf("operation=list_records:%u\n", MEM_SERVICE_WIRE_OP_LIST_RECORDS);
    printf("operation=metrics:%u\n", MEM_SERVICE_WIRE_OP_METRICS);
    printf("operation=export_snapshot:%u\n", MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT);
    printf("operation=export_snapshot_page:%u\n",
           MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE);
    printf("operation=restore_snapshot:%u\n", MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT);
    printf("operation=restore_snapshot_page:%u\n",
           MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE);
    printf("operation=put_object:%u\n", MEM_SERVICE_WIRE_OP_PUT_OBJECT);
    printf("operation=get_object:%u\n", MEM_SERVICE_WIRE_OP_GET_OBJECT);
    printf("operation=inspect_object:%u\n", MEM_SERVICE_WIRE_OP_INSPECT_OBJECT);
    printf("operation=register_prefix_entry:%u\n",
           MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY);
    printf("operation=lookup_prefix_entry:%u\n",
           MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY);
    printf("operation=publish_kv_segment:%u\n", MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT);
    printf("operation=resolve_kv_segment:%u\n", MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT);
    printf("operation=publish_runtime_handoff:%u\n",
           MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF);
    printf("operation=resolve_runtime_handoff:%u\n",
           MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF);
    printf("operation=register_execution_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT);
    printf("operation=query_execution_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT);
    printf("operation=register_training_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT);
    printf("operation=query_training_artifact:%u\n",
           MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT);
    printf("status=ok:%u\n", MEM_SERVICE_WIRE_STATUS_OK);
    printf("status=not_found:%u\n", MEM_SERVICE_WIRE_STATUS_NOT_FOUND);
    printf("status=stale_ref:%u\n", MEM_SERVICE_WIRE_STATUS_STALE_REF);
    printf("status=checksum_mismatch:%u\n",
           MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH);
    printf("status=version_conflict:%u\n",
           MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT);
    printf("status=invalid_model_binding:%u\n",
           MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING);
    printf("status=invalid_session:%u\n", MEM_SERVICE_WIRE_STATUS_INVALID_SESSION);
    printf("status=timeout:%u\n", MEM_SERVICE_WIRE_STATUS_TIMEOUT);
    printf("status=capacity_exceeded:%u\n",
           MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED);
    printf("status=unsupported:%u\n", MEM_SERVICE_WIRE_STATUS_UNSUPPORTED);
    printf("status=internal:%u\n", MEM_SERVICE_WIRE_STATUS_INTERNAL);
    return 0;
}

static int run_release_fixture_check(void)
{
    int failures = 0;

    if (MEM_SERVICE_WIRE_VERSION != 1U) {
        fprintf(stderr, "mem_service release-fixtures: wire_version mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_HEADER_LEN != 48U) {
        fprintf(stderr, "mem_service release-fixtures: wire_header_len mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U) {
        fprintf(stderr, "mem_service release-fixtures: wire_schema_version mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN == 0U ||
        MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: schema manifest fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT != 8U ||
        MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE != 9U ||
        MEM_SERVICE_WIRE_OP_PUT_OBJECT != 16U ||
        MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT != 97U) {
        fprintf(stderr, "mem_service release-fixtures: operation id mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_STATUS_STALE_REF != 2U ||
        MEM_SERVICE_WIRE_STATUS_INTERNAL != 10U) {
        fprintf(stderr, "mem_service release-fixtures: status id mismatch\n");
        failures -= 1;
    }
    if (strcmp(mem_service_default_unix_socket_spec(),
               "unix:" MEM_SERVICE_DEFAULT_UNIX_SOCKET) != 0) {
        fprintf(stderr, "mem_service release-fixtures: default endpoint mismatch\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service release-fixtures: status=ok manifest_version=1 "
           "public_headers=8 client_sources=2 examples=2 config_artifacts=3 "
           "metrics_export_formats=1 client_retry_policies=1 "
           "client_api_profiles=1 "
           "operations=22 statuses=11 "
           "schema_manifest_len=%u schema_manifest_checksum=0x%08x\n",
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM);
    return 0;
}

static const char *option_value(int argc, char **argv, const char *option_name)
{
    int i;

    for (i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], option_name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static bool option_present(int argc, char **argv, const char *option_name)
{
    int i;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], option_name) == 0) {
            return true;
        }
    }
    return false;
}

static int parse_socket_arg(int argc,
                            char **argv,
                            const char *option_name,
                            const char **socket_spec_out)
{
    int i;

    *socket_spec_out = NULL;
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], option_name) == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for %s\n", option_name);
                return -1;
            }
            *socket_spec_out = argv[++i];
        }
    }
    if (*socket_spec_out == NULL) {
        *socket_spec_out = mem_service_default_unix_socket_spec();
    }
    return 0;
}

static int parse_client_options(
    int argc,
    char **argv,
    struct mem_service_wire_client_options *options)
{
    int i;

    if (options == NULL) {
        return -1;
    }
    mem_service_wire_client_options_init(options);
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--timeout-ms") == 0) {
            char *end = NULL;
            unsigned long long parsed;

            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for --timeout-ms\n");
                return -1;
            }
            parsed = strtoull(argv[i + 1], &end, 0);
            if (end == argv[i + 1] || *end != '\0') {
                fprintf(stderr, "mem_service: invalid --timeout-ms value\n");
                return -1;
            }
            options->timeout_ms = (uint64_t)parsed;
            i += 1;
        } else if (strcmp(argv[i], "--max-attempts") == 0) {
            char *end = NULL;
            unsigned long long parsed;

            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for --max-attempts\n");
                return -1;
            }
            parsed = strtoull(argv[i + 1], &end, 0);
            if (end == argv[i + 1] || *end != '\0' || parsed == 0) {
                fprintf(stderr, "mem_service: invalid --max-attempts value\n");
                return -1;
            }
            options->max_attempts = (uint32_t)parsed;
            i += 1;
        } else if (strcmp(argv[i], "--retry-backoff-ms") == 0) {
            char *end = NULL;
            unsigned long long parsed;

            if (i + 1 >= argc) {
                fprintf(stderr, "mem_service: missing value for --retry-backoff-ms\n");
                return -1;
            }
            parsed = strtoull(argv[i + 1], &end, 0);
            if (end == argv[i + 1] || *end != '\0') {
                fprintf(stderr, "mem_service: invalid --retry-backoff-ms value\n");
                return -1;
            }
            options->retry_backoff_ms = (uint64_t)parsed;
            i += 1;
        } else if (strcmp(argv[i], "--retry-timeouts") == 0) {
            options->retry_on_timeout = 1U;
        }
    }
    return 0;
}

static int run_client_retry_fixture_check(void)
{
    char *argv[] = {
        "linqu_mem_service",
        "health",
        "--timeout-ms",
        "17",
        "--max-attempts",
        "3",
        "--retry-backoff-ms",
        "5",
        "--retry-timeouts",
    };
    struct mem_service_wire_client_options options;
    struct mem_service_wire_client_options defaults;

    mem_service_wire_client_options_init(&defaults);
    if (defaults.timeout_ms != 0 ||
        defaults.max_attempts != MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS ||
        defaults.retry_backoff_ms != 0 ||
        defaults.retry_on_timeout != 0) {
        fprintf(stderr, "mem_service client-retry-fixtures: default mismatch\n");
        return 1;
    }
    if (parse_client_options((int)(sizeof(argv) / sizeof(argv[0])),
                             argv,
                             &options) != 0 ||
        options.timeout_ms != 17U ||
        options.max_attempts != 3U ||
        options.retry_backoff_ms != 5U ||
        options.retry_on_timeout != 1U) {
        fprintf(stderr, "mem_service client-retry-fixtures: parsed mismatch\n");
        return 1;
    }
    printf("mem_service client-retry-fixtures: status=ok default_attempts=%u "
           "max_attempts=%u retry_backoff_ms=%" PRIu64 " retry_timeouts=%u\n",
           MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS,
           options.max_attempts,
           options.retry_backoff_ms,
           options.retry_on_timeout);
    return 0;
}

static int append_payload_field(char *payload,
                                size_t payload_len,
                                const char *name,
                                const char *value)
{
    return mem_service_wire_payload_append_field(payload, payload_len, name, value);
}

static int append_required_payload_field(char *payload,
                                         size_t payload_len,
                                         int argc,
                                         char **argv,
                                         const char *option_name,
                                         const char *field_name)
{
    const char *value = option_value(argc, argv, option_name);

    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "mem_service: missing required %s\n", option_name);
        return -1;
    }
    return append_payload_field(payload, payload_len, field_name, value);
}

static int append_optional_payload_field(char *payload,
                                         size_t payload_len,
                                         int argc,
                                         char **argv,
                                         const char *option_name,
                                         const char *field_name)
{
    return append_payload_field(payload, payload_len, field_name, option_value(argc, argv, option_name));
}

static int append_idempotency_payload_field(char *payload,
                                            size_t payload_len,
                                            int argc,
                                            char **argv)
{
    return append_optional_payload_field(payload,
                                         payload_len,
                                         argc,
                                         argv,
                                         "--idempotency-key",
                                         "idempotency_key");
}

struct mem_service_cli_config {
    bool has_listen;
    bool has_store;
    char listen[160];
    char store[512];
};

static void trim_ascii(char *value)
{
    char *start;
    char *end;
    size_t len;

    if (value == NULL) {
        return;
    }
    start = value;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start += 1;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1U);
    }
    len = strlen(value);
    while (len > 0) {
        end = value + len - 1U;
        if (!isspace((unsigned char)*end)) {
            break;
        }
        *end = '\0';
        len -= 1U;
    }
}

static bool parse_config_u64(const char *value)
{
    char *end = NULL;

    if (value == NULL || value[0] == '\0') {
        return false;
    }
    (void)strtoull(value, &end, 0);
    return end != value && *end == '\0';
}

static int copy_config_value(char *out, size_t out_len, const char *value)
{
    size_t value_len;

    if (out == NULL || out_len == 0 || value == NULL || value[0] == '\0') {
        return -1;
    }
    value_len = strlen(value);
    if (value_len >= out_len) {
        return -1;
    }
    memcpy(out, value, value_len + 1U);
    return 0;
}

static int apply_config_field(struct mem_service_cli_config *config,
                              const char *name,
                              const char *value)
{
    if (strcmp(name, "listen") == 0) {
        if (strncmp(value, "unix:", 5) != 0 ||
            copy_config_value(config->listen, sizeof(config->listen), value) != 0) {
            return -1;
        }
        config->has_listen = true;
        return 0;
    }
    if (strcmp(name, "store") == 0) {
        if (copy_config_value(config->store, sizeof(config->store), value) != 0) {
            return -1;
        }
        config->has_store = true;
        return 0;
    }
    if (strcmp(name, "backend") == 0) {
        return strcmp(value, "snapshot") == 0 ? 0 : -1;
    }
    if (strcmp(name, "auth_mode") == 0) {
        return strcmp(value, "none") == 0 ? 0 : -1;
    }
    if (strcmp(name, "metrics_mode") == 0) {
        return strcmp(value, "text-kv") == 0 ? 0 : -1;
    }
    if (strcmp(name, "adapter_enablement") == 0) {
        return strcmp(value, "core") == 0 || strcmp(value, "qwen3") == 0 ? 0 : -1;
    }
    if (strcmp(name, "max_records") == 0 ||
        strcmp(name, "max_payload_bytes") == 0) {
        return parse_config_u64(value) ? 0 : -1;
    }
    if (strcmp(name, "node_id") == 0 ||
        strcmp(name, "cluster_id") == 0 ||
        strcmp(name, "storage_root") == 0 ||
        strcmp(name, "retention") == 0) {
        return value[0] != '\0' ? 0 : -1;
    }
    return -1;
}

static int load_mem_service_config(const char *path,
                                   struct mem_service_cli_config *config,
                                   bool quiet)
{
    FILE *file;
    char line[768];
    uint64_t line_no = 0;

    if (path == NULL || path[0] == '\0' || config == NULL) {
        return -1;
    }
    memset(config, 0, sizeof(*config));
    file = fopen(path, "r");
    if (file == NULL) {
        if (!quiet) {
            fprintf(stderr, "mem_service: failed to open config %s\n", path);
        }
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals;
        char *name;
        char *value;

        line_no += 1U;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            if (!quiet) {
                fprintf(stderr,
                        "mem_service: config line too long path=%s line=%" PRIu64 "\n",
                        path,
                        line_no);
            }
            fclose(file);
            return -1;
        }
        trim_ascii(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        equals = strchr(line, '=');
        if (equals == NULL || equals == line) {
            if (!quiet) {
                fprintf(stderr,
                        "mem_service: invalid config line path=%s line=%" PRIu64 "\n",
                        path,
                        line_no);
            }
            fclose(file);
            return -1;
        }
        *equals = '\0';
        name = line;
        value = equals + 1;
        trim_ascii(name);
        trim_ascii(value);
        if (name[0] == '\0' || apply_config_field(config, name, value) != 0) {
            if (!quiet) {
                fprintf(stderr,
                        "mem_service: unsupported config field path=%s line=%" PRIu64
                        " field=%s\n",
                        path,
                        line_no,
                        name[0] != '\0' ? name : "<empty>");
            }
            fclose(file);
            return -1;
        }
    }
    if (ferror(file)) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    if (!config->has_listen) {
        if (!quiet) {
            fprintf(stderr, "mem_service: config missing required listen path=%s\n", path);
        }
        return -1;
    }
    return 0;
}

static int run_config_fixture_check(void)
{
    char valid_path[160];
    char invalid_path[160];
    struct mem_service_cli_config config;
    FILE *file;
    int failures = 0;

    snprintf(valid_path,
             sizeof(valid_path),
             "/tmp/linqu_mem_service_config_fixture_%ld.conf",
             (long)getpid());
    snprintf(invalid_path,
             sizeof(invalid_path),
             "/tmp/linqu_mem_service_config_fixture_%ld_bad.conf",
             (long)getpid());
    file = fopen(valid_path, "w");
    if (file == NULL) {
        return 1;
    }
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture.sock\n"
                "store=/tmp/linqu_mem_service_fixture.store\n"
                "node_id=fixture-node\n"
                "cluster_id=fixture-cluster\n"
                "storage_root=/tmp/linqu_mem_service_fixture\n"
                "backend=snapshot\n"
                "max_records=1024\n"
                "max_payload_bytes=4096\n"
                "retention=manual\n"
                "auth_mode=none\n"
                "metrics_mode=text-kv\n"
                "adapter_enablement=core\n") < 0) {
        fclose(file);
        unlink(valid_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        return 1;
    }
    file = fopen(invalid_path, "w");
    if (file == NULL) {
        unlink(valid_path);
        return 1;
    }
    if (fprintf(file, "listen=tcp:127.0.0.1:9900\nbackend=unknown\n") < 0) {
        if (file != NULL) {
            fclose(file);
        }
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    if (fclose(file) != 0) {
        unlink(valid_path);
        unlink(invalid_path);
        return 1;
    }
    if (load_mem_service_config(valid_path, &config, false) != 0 ||
        !config.has_listen ||
        !config.has_store ||
        strcmp(config.listen, "unix:/tmp/linqu_mem_service_fixture.sock") != 0 ||
        strcmp(config.store, "/tmp/linqu_mem_service_fixture.store") != 0) {
        failures -= 1;
    }
    if (load_mem_service_config(invalid_path, &config, true) == 0) {
        failures -= 1;
    }
    unlink(valid_path);
    unlink(invalid_path);
    if (failures != 0) {
        fprintf(stderr, "mem_service config-fixtures: failed\n");
        return 1;
    }
    printf("mem_service config-fixtures: status=ok schema_version=%u listen=%s store=%s\n",
           MEM_SERVICE_CONFIG_SCHEMA_VERSION,
           "unix:/tmp/linqu_mem_service_fixture.sock",
           "/tmp/linqu_mem_service_fixture.store");
    return 0;
}

static int run_serve(int argc, char **argv)
{
    const char *config_path = option_value(argc, argv, "--config");
    const char *listen_override = option_value(argc, argv, "--listen");
    const char *store_override = option_value(argc, argv, "--store");
    const char *listen_spec = mem_service_default_unix_socket_spec();
    const char *store_path = NULL;
    struct mem_service_cli_config config;

    if ((config_path == NULL && option_present(argc, argv, "--config")) ||
        parse_socket_arg(argc, argv, "--listen", &listen_spec) != 0) {
        return 2;
    }
    if (config_path != NULL) {
        if (load_mem_service_config(config_path, &config, false) != 0) {
            return 2;
        }
        listen_spec = config.has_listen ? config.listen : listen_spec;
        store_path = config.has_store ? config.store : NULL;
    }
    if (listen_override != NULL) {
        listen_spec = listen_override;
    }
    if (store_override != NULL) {
        store_path = store_override;
    }
    return mem_service_run_unix_daemon_with_store(listen_spec, store_path);
}

static int run_client_status(int argc,
                             char **argv,
                             enum mem_service_wire_operation operation,
                             const char *label)
{
    const char *connect_spec;
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char payload[128];
    int rc;

    memset(payload, 0, sizeof(payload));
    if (parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) {
        return 2;
    }
    rc = mem_service_send_unix_request_with_options(connect_spec,
                                                    &options,
                                                    operation,
                                                    NULL,
                                                    payload,
                                                    sizeof(payload),
                                                    &status);
    printf("mem_service %s: status=%s", label, mem_service_wire_status_name(status));
    if (payload[0] != '\0') {
        printf(" payload=%s", payload);
    }
    printf("\n");
    return rc;
}

static int run_client_payload_command(int argc,
                                      char **argv,
                                      enum mem_service_wire_operation operation,
                                      const char *label,
                                      const char *payload)
{
    const char *connect_spec;
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    int rc;

    memset(response, 0, sizeof(response));
    if (parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) {
        return 2;
    }
    rc = mem_service_send_unix_request_with_options(connect_spec,
                                                    &options,
                                                    operation,
                                                    payload,
                                                    response,
                                                    sizeof(response),
                                                    &status);
    printf("mem_service %s: status=%s", label, mem_service_wire_status_name(status));
    if (response[0] != '\0') {
        printf("\n%s", response);
        if (response[strlen(response) - 1] != '\n') {
            printf("\n");
        }
    } else {
        printf("\n");
    }
    return rc;
}

static int send_client_payload_request(int argc,
                                       char **argv,
                                       enum mem_service_wire_operation operation,
                                       const char *payload,
                                       char *response,
                                       size_t response_len,
                                       enum mem_service_wire_status *status_out)
{
    const char *connect_spec;
    struct mem_service_wire_client_options options;

    if (parse_socket_arg(argc, argv, "--connect", &connect_spec) != 0 ||
        parse_client_options(argc, argv, &options) != 0) {
        return 2;
    }
    if (response != NULL && response_len > 0) {
        response[0] = '\0';
    }
    return mem_service_send_unix_request_with_options(connect_spec,
                                                      &options,
                                                      operation,
                                                      payload,
                                                      response,
                                                      response_len,
                                                      status_out);
}

static bool metrics_export_key_is_safe(const char *key, size_t key_len)
{
    size_t i;

    if (key == NULL || key_len == 0) {
        return false;
    }
    for (i = 0; i < key_len; ++i) {
        unsigned char ch = (unsigned char)key[i];

        if (!(isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return true;
}

static const char *metrics_export_prometheus_type(const char *key, size_t key_len)
{
    static const char max_latency_key[] = "request_latency_max_ms";

    if (key_len == sizeof(max_latency_key) - 1U &&
        strncmp(key, max_latency_key, key_len) == 0) {
        return "gauge";
    }
    return "counter";
}

static int append_metrics_export_line(char *output,
                                      size_t output_len,
                                      size_t *used,
                                      const char *fmt,
                                      ...)
{
    va_list ap;
    int written;

    if (output == NULL || used == NULL || *used >= output_len) {
        return -1;
    }
    va_start(ap, fmt);
    written = vsnprintf(output + *used, output_len - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= output_len - *used) {
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static int render_metrics_prometheus_text(const char *metrics_payload,
                                          char *output,
                                          size_t output_len)
{
    const char *cursor = metrics_payload;
    size_t used = 0;

    if (metrics_payload == NULL || output == NULL || output_len == 0) {
        return -1;
    }
    output[0] = '\0';
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *equals;
        const char *value;
        size_t line_len;
        size_t key_len;
        size_t value_len;

        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        line_len = (size_t)(line_end - cursor);
        if (line_len == 0) {
            if (*line_end == '\0') {
                break;
            }
            cursor = line_end + 1;
            continue;
        }
        equals = memchr(cursor, '=', line_len);
        if (equals == NULL || equals == cursor || equals + 1 >= line_end) {
            return -1;
        }
        key_len = (size_t)(equals - cursor);
        value = equals + 1;
        value_len = (size_t)(line_end - value);
        if (!metrics_export_key_is_safe(cursor, key_len)) {
            return -1;
        }
        if (append_metrics_export_line(output,
                                       output_len,
                                       &used,
                                       "# TYPE lingqu_mem_service_%.*s %s\n",
                                       (int)key_len,
                                       cursor,
                                       metrics_export_prometheus_type(cursor,
                                                                      key_len)) != 0 ||
            append_metrics_export_line(output,
                                       output_len,
                                       &used,
                                       "lingqu_mem_service_%.*s %.*s\n",
                                       (int)key_len,
                                       cursor,
                                       (int)value_len,
                                       value) != 0) {
            return -1;
        }
        if (*line_end == '\0') {
            break;
        }
        cursor = line_end + 1;
    }
    return 0;
}

static int run_metrics_export_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=3\n"
        "ok_count=2\n"
        "request_latency_total_ms=11\n"
        "request_latency_max_ms=7\n";
    char exported[1024];

    if (render_metrics_prometheus_text(sample_metrics,
                                       exported,
                                       sizeof(exported)) != 0) {
        fprintf(stderr, "mem_service metrics-export-fixtures: render failed\n");
        return 1;
    }
    if (strstr(exported,
               "# TYPE lingqu_mem_service_request_count counter\n"
               "lingqu_mem_service_request_count 3\n") == NULL ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_latency_total_ms counter\n"
               "lingqu_mem_service_request_latency_total_ms 11\n") == NULL ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_latency_max_ms gauge\n"
               "lingqu_mem_service_request_latency_max_ms 7\n") == NULL) {
        fprintf(stderr,
                "mem_service metrics-export-fixtures: prometheus output mismatch\n");
        return 1;
    }
    if (render_metrics_prometheus_text("bad-key=1\n",
                                       exported,
                                       sizeof(exported)) == 0 ||
        render_metrics_prometheus_text("badline\n",
                                       exported,
                                       sizeof(exported)) == 0) {
        fprintf(stderr,
                "mem_service metrics-export-fixtures: invalid input accepted\n");
        return 1;
    }
    printf("mem_service metrics-export-fixtures: status=ok format=prometheus-text metrics=4\n");
    return 0;
}

static int run_metrics_export(int argc, char **argv)
{
    const char *format = option_value(argc, argv, "--format");
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char exported[16384];
    int rc;

    memset(response, 0, sizeof(response));
    memset(exported, 0, sizeof(exported));
    if (format == NULL) {
        format = "prometheus-text";
    }
    if (strcmp(format, "prometheus-text") != 0 &&
        strcmp(format, "prometheus") != 0) {
        fprintf(stderr, "mem_service: unsupported metrics export format %s\n", format);
        return 2;
    }
    rc = send_client_payload_request(argc,
                                     argv,
                                     MEM_SERVICE_WIRE_OP_METRICS,
                                     NULL,
                                     response,
                                     sizeof(response),
                                     &status);
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service: metrics export failed status=%s\n",
                mem_service_wire_status_name(status));
        return rc != 0 ? rc : 1;
    }
    if (render_metrics_prometheus_text(response, exported, sizeof(exported)) != 0) {
        fprintf(stderr, "mem_service: metrics export render failed\n");
        return 1;
    }
    printf("%s", exported);
    return 0;
}

static const char *snapshot_path_arg(int argc, char **argv, const char *option_name)
{
    const char *path = option_value(argc, argv, option_name);
    int i;

    if (path != NULL) {
        return path;
    }
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--connect") == 0 ||
            strcmp(argv[i], "--from") == 0 ||
            strcmp(argv[i], "--to") == 0 ||
            strcmp(argv[i], "--max-records") == 0 ||
            strcmp(argv[i], "--max-attempts") == 0 ||
            strcmp(argv[i], "--retry-backoff-ms") == 0 ||
            strcmp(argv[i], "--timeout-ms") == 0 ||
            strcmp(argv[i], "--idempotency-key") == 0) {
            i += 1;
            continue;
        }
        if (strcmp(argv[i], "--retry-timeouts") == 0) {
            continue;
        }
        if (argv[i][0] != '-') {
            return argv[i];
        }
    }
    return NULL;
}

static int run_put_object(int argc, char **argv)
{
    char payload[512] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backing-len", "backing_len") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--checksum", "checksum") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--version", "version") != 0 ||
        append_idempotency_payload_field(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc, argv, MEM_SERVICE_WIRE_OP_PUT_OBJECT, "put-object", payload);
}

static int run_get_object(int argc, char **argv)
{
    char payload[160] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0) {
        return 2;
    }
    return run_client_payload_command(argc, argv, MEM_SERVICE_WIRE_OP_GET_OBJECT, "get-object", payload);
}

static int run_inspect_object(int argc, char **argv)
{
    char payload[160] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_INSPECT_OBJECT,
                                      "inspect-object",
                                      payload);
}

static int run_export_snapshot_page(int argc, char **argv)
{
    char payload[160] = "";

    if (append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--start-index",
                                      "start_index") != 0 ||
        append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--max-records",
                                      "max_records") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                      "export-snapshot-page",
                                      payload);
}

static int write_snapshot_records_from_page(FILE *file, const char *response)
{
    const char *records = strstr(response, "record_begin\n");

    if (records == NULL) {
        return 0;
    }
    return fputs(records, file) < 0 ? -1 : 0;
}

static int run_export_snapshot_to(int argc, char **argv)
{
    const char *path = snapshot_path_arg(argc, argv, "--to");
    const char *max_records_arg = option_value(argc, argv, "--max-records");
    char tmp_path[512];
    char payload[160];
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    FILE *file;
    uint64_t start_index = 0;
    uint64_t page_count = 0;
    uint64_t record_count = 0;
    bool wrote_header = false;

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "mem_service: missing snapshot output path\n");
        return 2;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "mem_service: snapshot output path too long\n");
        return 2;
    }
    file = fopen(tmp_path, "w");
    if (file == NULL) {
        fprintf(stderr, "mem_service: failed to open snapshot output %s\n", tmp_path);
        return 1;
    }
    for (;;) {
        struct mem_service_wire_payload_view view;
        uint64_t next_index;
        uint32_t complete;
        int rc;

        payload[0] = '\0';
        if (mem_service_wire_payload_append_u64(payload,
                                                sizeof(payload),
                                                "start_index",
                                                start_index) != 0 ||
            append_payload_field(payload,
                                 sizeof(payload),
                                 "max_records",
                                 max_records_arg) != 0) {
            fclose(file);
            remove(tmp_path);
            return 2;
        }
        rc = send_client_payload_request(argc,
                                         argv,
                                         MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE,
                                         payload,
                                         response,
                                         sizeof(response),
                                         &status);
        if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
            fclose(file);
            remove(tmp_path);
            fprintf(stderr,
                    "mem_service: export-snapshot-page failed status=%s\n",
                    mem_service_wire_status_name(status));
            return rc != 0 ? rc : 1;
        }
        view = mem_service_wire_payload_view_from_cstr(response);
        if (!wrote_header) {
            record_count = mem_service_wire_payload_get_u64(&view, "record_count", 0);
            if (fprintf(file,
                        "%s\nrecord_count=%" PRIu64 "\n",
                        MEM_SERVICE_CLI_STORE_MAGIC,
                        record_count) < 0) {
                fclose(file);
                remove(tmp_path);
                return 1;
            }
            wrote_header = true;
        }
        if (write_snapshot_records_from_page(file, response) != 0) {
            fclose(file);
            remove(tmp_path);
            return 1;
        }
        complete = mem_service_wire_payload_get_u32(&view, "complete", 0);
        next_index = mem_service_wire_payload_get_u64(&view, "next_index", start_index);
        page_count += 1U;
        if (complete != 0) {
            break;
        }
        if (next_index <= start_index) {
            fclose(file);
            remove(tmp_path);
            fprintf(stderr, "mem_service: export-snapshot-page made no progress\n");
            return 1;
        }
        start_index = next_index;
    }
    if (fclose(file) != 0) {
        remove(tmp_path);
        return 1;
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        fprintf(stderr, "mem_service: failed to publish snapshot %s\n", path);
        return 1;
    }
    printf("mem_service export-snapshot-to: status=ok path=%s record_count=%" PRIu64
           " pages=%" PRIu64 "\n",
           path,
           record_count,
           page_count);
    return 0;
}

static const char *restore_snapshot_path_arg(int argc, char **argv)
{
    return snapshot_path_arg(argc, argv, "--from");
}

static void trim_snapshot_line(char *line)
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

static int read_snapshot_file(const char *path, char *payload, size_t payload_len)
{
    FILE *file;
    size_t used;
    int next;

    if (path == NULL || path[0] == '\0' || payload == NULL || payload_len == 0) {
        fprintf(stderr, "mem_service: missing snapshot path\n");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "mem_service: failed to open snapshot %s\n", path);
        return -1;
    }
    used = fread(payload, 1, payload_len - 1U, file);
    if (ferror(file)) {
        fclose(file);
        fprintf(stderr, "mem_service: failed to read snapshot %s\n", path);
        return -1;
    }
    next = fgetc(file);
    if (next != EOF) {
        fclose(file);
        return -2;
    }
    payload[used] = '\0';
    if (fclose(file) != 0) {
        fprintf(stderr, "mem_service: failed to close snapshot %s\n", path);
        return -1;
    }
    return 0;
}

static bool parse_snapshot_record_count(const char *line, uint64_t *record_count_out)
{
    char *end = NULL;
    uint64_t parsed;

    if (line == NULL || strncmp(line, "record_count=", 13) != 0 ||
        record_count_out == NULL) {
        return false;
    }
    parsed = strtoull(line + 13, &end, 10);
    if (end == line + 13 || *end != '\0') {
        return false;
    }
    *record_count_out = parsed;
    return true;
}

static int append_snapshot_text_line(char *out,
                                     size_t out_len,
                                     const char *line)
{
    size_t used = strlen(out);
    int written;

    if (used >= out_len) {
        return -1;
    }
    written = snprintf(out + used, out_len - used, "%s\n", line);
    if (written < 0 || (size_t)written >= out_len - used) {
        return -1;
    }
    return 0;
}

static int send_restore_snapshot_page_payload(int argc,
                                              char **argv,
                                              const char *payload,
                                              char *response,
                                              size_t response_len,
                                              enum mem_service_wire_status *status_out)
{
    return send_client_payload_request(argc,
                                       argv,
                                       MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE,
                                       payload,
                                       response,
                                       response_len,
                                       status_out);
}

static int begin_paged_restore_snapshot(int argc,
                                        char **argv,
                                        bool has_expected_records,
                                        uint64_t expected_records)
{
    char payload[128] = "action=begin\n";
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    if (has_expected_records &&
        mem_service_wire_payload_append_u64(payload,
                                            sizeof(payload),
                                            "expected_records",
                                            expected_records) != 0) {
        return 2;
    }
    rc = send_restore_snapshot_page_payload(argc,
                                            argv,
                                            payload,
                                            response,
                                            sizeof(response),
                                            &status);
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service: restore-snapshot begin failed status=%s\n",
                mem_service_wire_status_name(status));
        return rc != 0 ? rc : 1;
    }
    return 0;
}

static void cancel_paged_restore_snapshot(int argc, char **argv)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;

    (void)send_restore_snapshot_page_payload(argc,
                                             argv,
                                             "action=cancel\n",
                                             response,
                                             sizeof(response),
                                             &status);
}

static int send_restore_snapshot_records_page(int argc,
                                              char **argv,
                                              uint64_t page_index,
                                              bool complete,
                                              const char *records)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN + 1U];
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int written;
    int rc;

    written = snprintf(payload,
                       sizeof(payload),
                       "action=append\npage_index=%" PRIu64 "\ncomplete=%u\n%s",
                       page_index,
                       complete ? 1U : 0U,
                       records != NULL ? records : "");
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return 2;
    }
    rc = send_restore_snapshot_page_payload(argc,
                                            argv,
                                            payload,
                                            response,
                                            sizeof(response),
                                            &status);
    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {
        fprintf(stderr,
                "mem_service: restore-snapshot append failed status=%s page=%" PRIu64 "\n",
                mem_service_wire_status_name(status),
                page_index);
        return rc != 0 ? rc : 1;
    }
    return 0;
}

static int commit_paged_restore_snapshot(int argc, char **argv)
{
    char response[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc = send_restore_snapshot_page_payload(argc,
                                                argv,
                                                "action=commit\n",
                                                response,
                                                sizeof(response),
                                                &status);

    printf("mem_service restore-snapshot: status=%s", mem_service_wire_status_name(status));
    if (response[0] != '\0') {
        printf("\n%s", response);
        if (response[strlen(response) - 1] != '\n') {
            printf("\n");
        }
    } else {
        printf("\n");
    }
    return rc;
}

static int append_record_to_restore_page(int argc,
                                         char **argv,
                                         char *page_records,
                                         size_t page_records_len,
                                         const char *record,
                                         uint64_t *page_index,
                                         uint64_t *pages_sent)
{
    size_t page_used = strlen(page_records);
    size_t record_len = strlen(record);

    if (record_len + 128U >= MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        fprintf(stderr, "mem_service: snapshot record exceeds wire payload capacity\n");
        return 2;
    }
    if (page_used > 0 &&
        page_used + record_len + 128U >= MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        int rc = send_restore_snapshot_records_page(argc,
                                                    argv,
                                                    *page_index,
                                                    false,
                                                    page_records);

        if (rc != 0) {
            return rc;
        }
        *page_index += 1U;
        *pages_sent += 1U;
        page_records[0] = '\0';
        page_used = 0;
    }
    if (page_used + record_len >= page_records_len) {
        return 2;
    }
    memcpy(page_records + page_used, record, record_len + 1U);
    return 0;
}

static int run_restore_snapshot_paged(int argc, char **argv, const char *path)
{
    FILE *file;
    char line[512];
    char record[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char page_records[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    bool saw_magic = false;
    bool began = false;
    bool in_record = false;
    bool has_expected_records = false;
    uint64_t expected_records = 0;
    uint64_t page_index = 0;
    uint64_t pages_sent = 0;
    int rc = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "mem_service: failed to open snapshot %s\n", path);
        return 1;
    }
    record[0] = '\0';
    page_records[0] = '\0';
    while (fgets(line, sizeof(line), file) != NULL) {
        trim_snapshot_line(line);
        if (!saw_magic) {
            if (strcmp(line, MEM_SERVICE_CLI_STORE_MAGIC) != 0) {
                fclose(file);
                return 2;
            }
            saw_magic = true;
            continue;
        }
        if (!began && !in_record) {
            if (parse_snapshot_record_count(line, &expected_records)) {
                has_expected_records = true;
                continue;
            }
            if (strcmp(line, "record_begin") != 0) {
                continue;
            }
            rc = begin_paged_restore_snapshot(argc,
                                              argv,
                                              has_expected_records,
                                              expected_records);
            if (rc != 0) {
                fclose(file);
                return rc;
            }
            began = true;
        }
        if (strcmp(line, "record_begin") == 0) {
            if (in_record) {
                rc = 2;
                break;
            }
            record[0] = '\0';
            in_record = true;
        }
        if (in_record &&
            append_snapshot_text_line(record, sizeof(record), line) != 0) {
            rc = 2;
            break;
        }
        if (strcmp(line, "record_end") == 0) {
            rc = append_record_to_restore_page(argc,
                                               argv,
                                               page_records,
                                               sizeof(page_records),
                                               record,
                                               &page_index,
                                               &pages_sent);
            if (rc != 0) {
                break;
            }
            record[0] = '\0';
            in_record = false;
        }
    }
    if (rc == 0 && (!saw_magic || in_record)) {
        rc = 2;
    }
    if (rc == 0 && !began) {
        rc = begin_paged_restore_snapshot(argc,
                                          argv,
                                          has_expected_records,
                                          expected_records);
        began = rc == 0;
    }
    if (rc == 0 && page_records[0] != '\0') {
        rc = send_restore_snapshot_records_page(argc,
                                                argv,
                                                page_index,
                                                true,
                                                page_records);
        if (rc == 0) {
            page_index += 1U;
            pages_sent += 1U;
            page_records[0] = '\0';
        }
    }
    if (rc == 0 && page_records[0] == '\0' && !has_expected_records) {
        rc = send_restore_snapshot_records_page(argc, argv, page_index, true, "");
        if (rc == 0) {
            pages_sent += 1U;
        }
    }
    if (fclose(file) != 0 && rc == 0) {
        rc = 1;
    }
    if (rc != 0) {
        if (began) {
            cancel_paged_restore_snapshot(argc, argv);
        }
        return rc;
    }
    return commit_paged_restore_snapshot(argc, argv);
}

static int run_restore_snapshot(int argc, char **argv)
{
    char payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN + 1U];
    const char *path = restore_snapshot_path_arg(argc, argv);
    int read_rc;

    memset(payload, 0, sizeof(payload));
    read_rc = read_snapshot_file(path, payload, sizeof(payload));
    if (read_rc == -2) {
        return run_restore_snapshot_paged(argc, argv, path);
    }
    if (read_rc != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT,
                                      "restore-snapshot",
                                      payload);
}

static int append_block_context_payload(char *payload,
                                        size_t payload_len,
                                        int argc,
                                        char **argv,
                                        bool require_result)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--request-id", "request_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--prefix-group", "prefix_group") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--group-id", "group_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--block-hash", "block_hash") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--placement-node", "placement_node") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--placement-level", "placement_level") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--hot-segment", "hot_segment_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--state", "state") != 0 ||
        append_idempotency_payload_field(payload, payload_len, argc, argv) != 0) {
        return -1;
    }
    if (require_result) {
        return append_required_payload_field(payload,
                                             payload_len,
                                             argc,
                                             argv,
                                             "--result-segment",
                                             "result_segment_id");
    }
    return append_optional_payload_field(payload,
                                         payload_len,
                                         argc,
                                         argv,
                                         "--result-segment",
                                         "result_segment_id");
}

static int run_register_prefix(int argc, char **argv)
{
    char payload[768] = "";

    if (append_block_context_payload(payload, sizeof(payload), argc, argv, true) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY,
                                      "register-prefix",
                                      payload);
}

static int run_lookup_prefix(int argc, char **argv)
{
    char payload[256] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--request-id", "request_id") != 0 ||
        append_required_payload_field(payload, sizeof(payload), argc, argv, "--prefix-group", "prefix_group") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY,
                                      "lookup-prefix",
                                      payload);
}

static int run_publish_kv(int argc, char **argv)
{
    char payload[768] = "";

    if (append_block_context_payload(payload, sizeof(payload), argc, argv, false) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT,
                                      "publish-kv",
                                      payload);
}

static int run_resolve_kv(int argc, char **argv)
{
    char payload[192] = "";

    if (append_payload_field(payload, sizeof(payload), "key", option_value(argc, argv, "--key")) != 0 ||
        append_payload_field(payload, sizeof(payload), "block_hash", option_value(argc, argv, "--block-hash")) != 0 ||
        payload[0] == '\0') {
        fprintf(stderr, "mem_service: missing required --key or --block-hash\n");
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT,
                                      "resolve-kv",
                                      payload);
}

static int append_artifact_payload(char *payload,
                                   size_t payload_len,
                                   int argc,
                                   char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--session-id", "session_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--request-id", "request_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--model-key", "model_key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--artifact-kind", "artifact_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--artifact-id", "artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-len", "backing_len") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--checksum", "checksum") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--version", "version") != 0 ||
        append_idempotency_payload_field(payload, payload_len, argc, argv) != 0) {
        return -1;
    }
    return 0;
}

static int append_artifact_query_payload(char *payload,
                                         size_t payload_len,
                                         int argc,
                                         char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-session-id", "expected_session_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-model-key", "expected_model_key") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-artifact-kind", "expected_artifact_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-artifact-id", "expected_artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-version", "expected_version") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-checksum", "expected_checksum") != 0) {
        return -1;
    }
    return 0;
}

static int run_publish_runtime_handoff(int argc, char **argv)
{
    char payload[768] = "";

    if (append_artifact_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF,
                                      "publish-runtime-handoff",
                                      payload);
}

static int run_resolve_runtime_handoff(int argc, char **argv)
{
    char payload[512] = "";

    if (append_artifact_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF,
                                      "resolve-runtime-handoff",
                                      payload);
}

static int run_register_execution_artifact(int argc, char **argv)
{
    char payload[768] = "";

    if (append_artifact_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT,
                                      "register-execution-artifact",
                                      payload);
}

static int run_query_execution_artifact(int argc, char **argv)
{
    char payload[512] = "";

    if (append_artifact_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT,
                                      "query-execution-artifact",
                                      payload);
}

static int run_register_training_artifact(int argc, char **argv)
{
    char payload[768] = "";

    if (append_artifact_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                      "register-training-artifact",
                                      payload);
}

static int run_query_training_artifact(int argc, char **argv)
{
    char payload[512] = "";

    if (append_artifact_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                      "query-training-artifact",
                                      payload);
}

int main(int argc, char **argv)
{
    if (argc == 1 ||
        strcmp(argv[1], "--smoke") == 0 ||
        strcmp(argv[1], "--self-test") == 0) {
        return run_smoke();
    }
    if (strcmp(argv[1], "wire-fixtures") == 0) {
        return mem_service_run_wire_fixture_check();
    }
    if (strcmp(argv[1], "wire-schema") == 0) {
        return run_wire_schema_manifest();
    }
    if (strcmp(argv[1], "wire-schema-fixtures") == 0) {
        return run_wire_schema_fixture_check();
    }
    if (strcmp(argv[1], "store-fixtures") == 0) {
        return mem_service_run_store_fixture_check();
    }
    if (strcmp(argv[1], "config-fixtures") == 0) {
        return run_config_fixture_check();
    }
    if (strcmp(argv[1], "metrics-export-fixtures") == 0) {
        return run_metrics_export_fixture_check();
    }
    if (strcmp(argv[1], "client-retry-fixtures") == 0) {
        return run_client_retry_fixture_check();
    }
    if (strcmp(argv[1], "release-manifest") == 0) {
        return run_release_manifest();
    }
    if (strcmp(argv[1], "release-fixtures") == 0) {
        return run_release_fixture_check();
    }
    if (strcmp(argv[1], "serve") == 0) {
        return run_serve(argc, argv);
    }
    if (strcmp(argv[1], "health") == 0) {
        return run_client_status(argc, argv, MEM_SERVICE_WIRE_OP_HEALTH, "health");
    }
    if (strcmp(argv[1], "ready") == 0) {
        return run_client_status(argc, argv, MEM_SERVICE_WIRE_OP_READY, "ready");
    }
    if (strcmp(argv[1], "status") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_STATUS,
                                          "status",
                                          NULL);
    }
    if (strcmp(argv[1], "list-records") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_LIST_RECORDS,
                                          "list-records",
                                          NULL);
    }
    if (strcmp(argv[1], "metrics") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_METRICS,
                                          "metrics",
                                          NULL);
    }
    if (strcmp(argv[1], "metrics-export") == 0) {
        return run_metrics_export(argc, argv);
    }
    if (strcmp(argv[1], "export-snapshot") == 0) {
        return run_client_payload_command(argc,
                                          argv,
                                          MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT,
                                          "export-snapshot",
                                          NULL);
    }
    if (strcmp(argv[1], "export-snapshot-page") == 0) {
        return run_export_snapshot_page(argc, argv);
    }
    if (strcmp(argv[1], "export-snapshot-to") == 0) {
        return run_export_snapshot_to(argc, argv);
    }
    if (strcmp(argv[1], "restore-snapshot") == 0) {
        return run_restore_snapshot(argc, argv);
    }
    if (strcmp(argv[1], "put-object") == 0) {
        return run_put_object(argc, argv);
    }
    if (strcmp(argv[1], "get-object") == 0) {
        return run_get_object(argc, argv);
    }
    if (strcmp(argv[1], "inspect-object") == 0) {
        return run_inspect_object(argc, argv);
    }
    if (strcmp(argv[1], "register-prefix") == 0) {
        return run_register_prefix(argc, argv);
    }
    if (strcmp(argv[1], "lookup-prefix") == 0) {
        return run_lookup_prefix(argc, argv);
    }
    if (strcmp(argv[1], "publish-kv") == 0) {
        return run_publish_kv(argc, argv);
    }
    if (strcmp(argv[1], "resolve-kv") == 0) {
        return run_resolve_kv(argc, argv);
    }
    if (strcmp(argv[1], "publish-runtime-handoff") == 0) {
        return run_publish_runtime_handoff(argc, argv);
    }
    if (strcmp(argv[1], "resolve-runtime-handoff") == 0) {
        return run_resolve_runtime_handoff(argc, argv);
    }
    if (strcmp(argv[1], "register-execution-artifact") == 0) {
        return run_register_execution_artifact(argc, argv);
    }
    if (strcmp(argv[1], "query-execution-artifact") == 0) {
        return run_query_execution_artifact(argc, argv);
    }
    if (strcmp(argv[1], "register-training-artifact") == 0) {
        return run_register_training_artifact(argc, argv);
    }
    if (strcmp(argv[1], "query-training-artifact") == 0) {
        return run_query_training_artifact(argc, argv);
    }
    if (strcmp(argv[1], "--inspect-qwen3") == 0) {
#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
        return inspect_qwen3();
#else
        fprintf(stderr, "mem_service qwen3: inspect is available only in the qwen3 adapter build\n");
        return 2;
#endif
    }
    usage(argv[0]);
    return 2;
}
