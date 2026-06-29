#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "components/mem_service/mem_service_core.h"
#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_daemon.h"
#include "components/mem_service/mem_service_wire_client.h"
#include "components/mem_service/mem_service_wire_payload.h"
#include "components/mem_service/mem_service_wire_schema.h"

#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT
#include "components/llm_infer/llm_infer.h"
#endif

#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION 1U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN 9416U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM 0xf4cf34c6U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT 23U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT 113U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT 1U
#define MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT 2U
#define MEM_SERVICE_CONFIG_SCHEMA_VERSION 1U
#define MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION 1U
#define MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_VERSION 1U
#define MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN 6624U
#define MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM 0x7021f4cfU
#define MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_VERSION 1U
#define MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN 2019U
#define MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM 0xf7943816U
#define MEM_SERVICE_ALERT_RULES_VERSION 1U
#define MEM_SERVICE_ALERT_RULES_EXPECTED_LEN 1733U
#define MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM 0xbdff2246U
#define MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT 5U
#define MEM_SERVICE_OPS_CERTIFICATION_POLICY_VERSION 1U
#define MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN 1118U
#define MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM 0xe77c644bU
#define MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION 1U
#define MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION 1U
#define MEM_SERVICE_PACKAGE_MANIFEST_VERSION 1U
#define MEM_SERVICE_RELEASE_VERSION "0.1.0"
#define MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN 7238U
#define MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM 0x7d247471U
#define MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT 45U
#define MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT 26U
#define MEM_SERVICE_PACKAGE_TARBALL_NAME "linqu_mem_service-installed-layout-v1.tar"
#define MEM_SERVICE_NATIVE_DEB_NAME "linqu-mem-service_0.1.0-1_arm64.deb"
#define MEM_SERVICE_NATIVE_RPM_NAME "linqu-mem-service-0.1.0-1.aarch64.rpm"
#define MEM_SERVICE_API_ABI_POLICY_VERSION 1U
#define MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN 856U
#define MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM 0x5d95ae02U
#define MEM_SERVICE_COMPAT_MATRIX_VERSION 1U
#define MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN 1978U
#define MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM 0x61d07124U
#define MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT 11U
#define MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN 1251U
#define MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM 0x1e017705U
#define MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN 1733U
#define MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM 0x627bf6a1U
#define MEM_SERVICE_CLI_STORE_MAGIC "mem_service_store_v1"

static void usage(const char *argv0)
{
    printf("Usage: %s [--smoke] [--self-test]", argv0);
    printf(" [version] [version-fixtures]");
    printf(" [wire-fixtures] [wire-schema] [wire-schema-fixtures]");
    printf(" [store-fixtures] [journal-fixtures] [journal-compaction-fixtures] [journal-torn-recovery-fixtures] [config-fixtures]");
    printf(" [restore-policy-fixtures]");
    printf(" [metrics-export-fixtures] [collector-fixtures] [deployment-fixtures]");
    printf(" [admin-output-schema] [admin-output-fixtures]");
    printf(" [upgrade-rollback-policy] [upgrade-rollback-fixtures]");
    printf(" [upgrade-rollback-runtime-fixtures]");
    printf(" [alert-rules] [alert-fixtures] [alert-integration-fixtures]");
    printf(" [ops-certification-policy] [ops-certification-fixtures]");
    printf(" [ops-certification-evidence-fixtures]");
    printf(" [ops-certification-generate-evidence --rpm-file <path> --upgrade-rollback-marker <path>]");
    printf(" [ops-certification-linux-ci-smoke --rpm-file <path> --upgrade-rollback-marker <path> --evidence-file <path>]");
    printf(" [ops-certification-verify --evidence-file <path>]");
    printf(" [package-manifest] [package-fixtures]");
    printf(" [durable-catalog-fixtures]");
    printf(" [chunked-block-fixtures]");
    printf(" [transport-block-fixtures]");
    printf(" [network-transport-block-fixtures]");
    printf(" [remote-block-backend-policy-fixtures]");
    printf(" [remote-transport-evidence-fixtures]");
    printf(" [remote-transport-generate-evidence --source tcp:<ipv4>:<port> --producer-host <host> --consumer-host <host> --network-partition-marker <path> --evidence-file <path>]");
    printf(" [remote-transport-verify --evidence-file <path>]");
    printf(" [api-abi-policy] [api-abi-fixtures]");
    printf(" [client-retry-fixtures] [compat-matrix] [compat-fixtures]");
    printf(" [compat-baseline-v1] [compat-baseline-fixtures]");
    printf(" [compat-old-new-matrix] [compat-old-new-fixtures]");
    printf(" [compat-runtime-fixtures]");
    printf(" [compat-old-server-runtime-fixtures]");
    printf(" [serving-fail-closed-fixtures]");
    printf(" [pretraining-fail-closed-fixtures]");
    printf(" [typed-payload-fixtures]");
    printf(" [release-manifest] [release-fixtures]");
    printf(" [serve [--config <path>] [--listen unix:%s] [--store <path>]"
           " [--metrics-listen tcp:127.0.0.1:9900]]",
           MEM_SERVICE_DEFAULT_UNIX_SOCKET);
    printf(" [health|ready|status|list-records|metrics|metrics-export|audit-log|export-snapshot|export-snapshot-page|export-snapshot-to|restore-snapshot [--connect unix:%s] [--timeout-ms <ms>] [--max-attempts <n>] [--retry-backoff-ms <ms>] [--retry-timeouts]]",
           MEM_SERVICE_DEFAULT_UNIX_SOCKET);
    printf(" [metrics-export accepts --format prometheus-text]");
    printf(" [put-object|get-object|inspect-object|register-prefix|lookup-prefix|publish-kv|resolve-kv]");
    printf(" [publish-runtime-handoff|resolve-runtime-handoff]");
    printf(" [register-execution-artifact|query-execution-artifact]");
    printf(" [register-training-artifact|query-training-artifact]");
    printf(" [commit-training-step|resolve-training-step]");
    printf(" [mutating commands accept --idempotency-key <key>]");
    printf(" [object/artifact mutating commands accept --payload-file <path>]");
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

static const char *option_value(int argc, char **argv, const char *option_name);

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

static int render_api_abi_policy(char *policy, size_t policy_len, size_t *used_out)
{
    size_t used = 0;

    if (policy == NULL || policy_len == 0) {
        return -1;
    }
    policy[0] = '\0';
    if (append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "mem_service_api_abi_policy_version=%u\n",
                                MEM_SERVICE_API_ABI_POLICY_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_api_version=%u\n",
                                MEM_SERVICE_CLIENT_API_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_abi_version=%u\n",
                                MEM_SERVICE_CLIENT_ABI_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_api_compatibility=%s\n",
                                MEM_SERVICE_CLIENT_API_COMPATIBILITY) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_abi_compatibility=%s\n",
                                MEM_SERVICE_CLIENT_ABI_COMPATIBILITY) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_record_abi_size=%u\n",
                                MEM_SERVICE_CLIENT_RECORD_ABI_SIZE) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_record_actual_size=%zu\n",
                                sizeof(struct mem_service_client_record)) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_key_len=%u\n",
                                MEM_SERVICE_CLIENT_KEY_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_id_len=%u\n",
                                MEM_SERVICE_CLIENT_ID_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "client_state_len=%u\n",
                                MEM_SERVICE_CLIENT_STATE_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_min=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_max=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "unknown_field_policy=ignored_when_optional\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_field_policy=missing_required_field_fails_schema\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "operation_id_policy=stable-within-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "status_code_policy=stable-within-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "old_client_new_server_policy=compatible-within-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "new_client_old_server_policy=certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rollback_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "binary_typed_schema=typed-binary-v1\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_api_abi_policy(void)
{
    char policy[4096];
    size_t used = 0;

    if (render_api_abi_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service api-abi-policy: render failed\n");
        return 1;
    }
    (void)used;
    fputs(policy, stdout);
    return 0;
}

static int run_api_abi_fixture_check(void)
{
    char policy[4096];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_api_abi_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service api-abi-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(policy, used);
    if (used != MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service api-abi-fixtures: policy len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service api-abi-fixtures: policy checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (MEM_SERVICE_CLIENT_API_VERSION != 1U ||
        MEM_SERVICE_CLIENT_ABI_VERSION != 1U ||
        MEM_SERVICE_CLIENT_RECORD_ABI_SIZE !=
            sizeof(struct mem_service_client_record) ||
        MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_HEADER_LEN != 48U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U) {
        fprintf(stderr, "mem_service api-abi-fixtures: version/layout mismatch\n");
        failures -= 1;
    }
    if (strstr(policy, "old_client_new_server_policy=compatible-within-v1\n") ==
            NULL ||
        strstr(policy,
               "new_client_old_server_policy=certified\n") ==
            NULL ||
        strstr(policy, "upgrade_policy=current-version-only\n") == NULL ||
        strstr(policy, "rollback_policy=current-version-only\n") == NULL) {
        fprintf(stderr, "mem_service api-abi-fixtures: required policy missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service api-abi-fixtures: status=ok policy_version=%u "
           "policy_len=%u policy_checksum=0x%08x client_api_version=%u "
           "client_abi_version=%u client_record_abi_size=%u\n",
           MEM_SERVICE_API_ABI_POLICY_VERSION,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_CLIENT_API_VERSION,
           MEM_SERVICE_CLIENT_ABI_VERSION,
           MEM_SERVICE_CLIENT_RECORD_ABI_SIZE);
    return 0;
}

static int render_upgrade_rollback_policy(char *policy,
                                          size_t policy_len,
                                          size_t *used_out)
{
    size_t used = 0;

    if (policy == NULL || policy_len == 0) {
        return -1;
    }
    policy[0] = '\0';
    if (append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "mem_service_upgrade_rollback_policy_version=%u\n",
                                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "release_manifest_version=1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_version_current=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "admin_output_schema_len=%u\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "admin_output_schema_checksum=0x%08x\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "api_abi_policy_len=%u\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "api_abi_policy_checksum=0x%08x\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_baseline_len=%u\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_baseline_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_old_new_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "compat_old_new_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "config_schema_version=%u\n",
                                MEM_SERVICE_CONFIG_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "deployment_smoke_version=%u\n",
                                MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "store_magic=%s\n",
                                MEM_SERVICE_CLI_STORE_MAGIC) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "catalog_layout=storage-root-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "same_version_restart_recovery=store-snapshot+journal\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "same_version_restore=export-snapshot-page+restore-snapshot-page\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "same_version_runtime_gate=upgrade-rollback-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rollback_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "upgrade_admission=reject-unknown-release-generation\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rollback_admission=reject-unknown-release-generation\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "old_server_runtime_binary=certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "new_client_old_server=certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "catalog_schema_version=1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "migration_policy=catalog-schema-version-accept-current-reject-future\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "downgrade_policy=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=wire-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=wire-schema-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=admin-output-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=upgrade-rollback-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=api-abi-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=compat-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=compat-old-new-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=store-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=journal-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=durable-catalog-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=collector-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=alert-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=alert-integration-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=package-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=release-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=host-artifact-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_gate=install-smoke\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_upgrade_rollback_policy(void)
{
    char policy[4096];
    size_t used = 0;

    if (render_upgrade_rollback_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service upgrade-rollback-policy: render failed\n");
        return 1;
    }
    (void)used;
    fputs(policy, stdout);
    return 0;
}

static int run_upgrade_rollback_fixture_check(void)
{
    char policy[4096];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_upgrade_rollback_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service upgrade-rollback-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(policy, used);
    if (used != MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: policy len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: policy checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U ||
        MEM_SERVICE_CONFIG_SCHEMA_VERSION != 1U ||
        MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN == 0U ||
        MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN == 0U) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: release dependency missing\n");
        failures -= 1;
    }
    if (strstr(policy, "upgrade_policy=current-version-only\n") == NULL ||
        strstr(policy, "rollback_policy=current-version-only\n") == NULL ||
        strstr(policy, "old_server_runtime_binary=certified\n") == NULL ||
        strstr(policy, "new_client_old_server=certified\n") ==
            NULL ||
        strstr(policy, "same_version_runtime_gate=upgrade-rollback-runtime-fixtures\n") ==
            NULL ||
        strstr(policy, "required_gate=admin-output-fixtures\n") == NULL ||
        strstr(policy, "required_gate=upgrade-rollback-runtime-fixtures\n") == NULL ||
        strstr(policy, "required_gate=compat-runtime-fixtures\n") == NULL ||
        strstr(policy, "required_gate=alert-fixtures\n") == NULL ||
        strstr(policy, "required_gate=alert-integration-fixtures\n") == NULL ||
        strstr(policy, "required_gate=package-fixtures\n") == NULL ||
        strstr(policy, "required_gate=install-smoke\n") == NULL) {
        fprintf(stderr,
                "mem_service upgrade-rollback-fixtures: required policy missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service upgrade-rollback-fixtures: status=ok policy_version=%u "
           "policy_len=%u policy_checksum=0x%08x "
           "upgrade_policy=current-version-only "
           "rollback_policy=current-version-only required_gates=19\n",
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_VERSION,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM);
    return 0;
}

static int render_compat_matrix(char *matrix, size_t matrix_len, size_t *used_out)
{
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    if (matrix == NULL || matrix_len == 0) {
        return -1;
    }
    matrix[0] = '\0';
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    if (append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "mem_service_compat_matrix_version=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_scope=wire-schema,release-layout,client-retry,idempotency,audit,snapshot,journal\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_min=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_max=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_min=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_current=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "operation_count=%zu\n",
                                wire_schema_operation_count()) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "field_count=%zu\n",
                                field_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_count=%zu\n",
                                oneof_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_field_count=%zu\n",
                                oneof_field_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "status_count=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "unknown_text_field_policy=ignored_by_schema_validation\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "required_field_policy=missing_required_field_fails_schema\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_policy=at_least_one_selector_field_required\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "client_retry_policy=explicit-max-attempts-backoff\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "retry_timeout_policy=opt-in-retry-timeouts\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_scope=mutating-object-prefix-kv-runtime-execution-training\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_replay_match=operation-and-request-checksum\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_conflict_status=version_conflict\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "idempotency_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "audit_log_scope=mutating-and-fail-closed\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "audit_log_retention=bounded-ring\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "audit_log_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_store_magic=mem_service_journal_v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_path_policy=store-path-dot-journal\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_scope=completed-idempotency-and-audit-events\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "journal_truncation_policy=threshold-compaction\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "snapshot_store_magic=%s\n",
                                MEM_SERVICE_CLI_STORE_MAGIC) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "snapshot_full_restore_state=records-idempotency-audit\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "snapshot_paged_restore_state=records-only-clears-idempotency-audit\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "client_api=pretraining-refs-v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "client_api=pretraining-step-commit-v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=wire-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=wire-schema-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=store-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=journal-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=config-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=metrics-export-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=client-retry-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=release-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_test=daemon-runtime\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "release_gate=install-smoke\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "rollback_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "binary_typed_schema=typed-binary-v1\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_compat_matrix(void)
{
    char matrix[8192];
    size_t used = 0;

    if (render_compat_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-matrix: render failed\n");
        return 1;
    }
    (void)used;
    fputs(matrix, stdout);
    return 0;
}

static int run_compat_fixture_check(void)
{
    char matrix[8192];
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;
    uint32_t checksum;
    int failures = 0;
    const struct mem_service_wire_operation_schema *put_object_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_PUT_OBJECT);
    const struct mem_service_wire_operation_schema *resolve_kv_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT);
    struct mem_service_wire_payload_view valid_put =
        mem_service_wire_payload_view_from_cstr(
            "key=compat-object\n"
            "version=1\n"
            "checksum=2\n"
            "unknown_future_field=ignored\n");
    struct mem_service_wire_payload_view invalid_put =
        mem_service_wire_payload_view_from_cstr("version=1\nchecksum=2\n");
    struct mem_service_wire_payload_view invalid_oneof =
        mem_service_wire_payload_view_from_cstr("unknown_future_field=ignored\n");

    if (render_compat_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-fixtures: render failed\n");
        return 1;
    }
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    checksum = mem_service_wire_checksum(matrix, used);
    if (used != MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service compat-fixtures: matrix len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service compat-fixtures: matrix checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_HEADER_LEN != 48U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U ||
        wire_schema_operation_count() !=
            MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT ||
        field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT ||
        oneof_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_COUNT ||
        oneof_field_count != MEM_SERVICE_WIRE_SCHEMA_MANIFEST_ONEOF_FIELD_COUNT ||
        MEM_SERVICE_WIRE_OP_AUDIT_LOG != 10U ||
        MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT != 4U) {
        fprintf(stderr, "mem_service compat-fixtures: version/id matrix mismatch\n");
        failures -= 1;
    }
    if (!mem_service_wire_schema_validate_payload(put_object_schema,
                                                  &valid_put,
                                                  NULL) ||
        mem_service_wire_schema_validate_payload(put_object_schema,
                                                 &invalid_put,
                                                 NULL) ||
        mem_service_wire_schema_validate_payload(resolve_kv_schema,
                                                 &invalid_oneof,
                                                 NULL)) {
        fprintf(stderr, "mem_service compat-fixtures: schema policy mismatch\n");
        failures -= 1;
    }
    if (strstr(matrix, "idempotency_conflict_status=version_conflict\n") == NULL ||
        strstr(matrix, "idempotency_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(matrix, "audit_log_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(matrix, "compat_test=journal-fixtures\n") == NULL ||
        strstr(matrix, "compat_test=compat-runtime-fixtures\n") == NULL ||
        strstr(matrix,
               "snapshot_paged_restore_state=records-only-clears-idempotency-audit\n") ==
            NULL ||
        strstr(matrix, "upgrade_policy=current-version-only\n") == NULL) {
        fprintf(stderr, "mem_service compat-fixtures: required rule missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service compat-fixtures: status=ok matrix_version=%u "
           "matrix_len=%u matrix_checksum=0x%08x wire_version=%u "
           "schema_version=%u operations=%u fields=%u statuses=%u\n",
           MEM_SERVICE_COMPAT_MATRIX_VERSION,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM,
           MEM_SERVICE_WIRE_VERSION,
           MEM_SERVICE_WIRE_SCHEMA_VERSION,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT,
           MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT);
    return 0;
}

static int render_compat_baseline_v1(char *baseline,
                                     size_t baseline_len,
                                     size_t *used_out)
{
    size_t used = 0;

    if (baseline == NULL || baseline_len == 0) {
        return -1;
    }
    baseline[0] = '\0';
    if (append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "mem_service_compat_baseline_version=1\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_name=mem-service-wire-v1\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_scope=old-v1-client-to-current-server\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_version=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_header_len=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "compat_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "compat_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "operation_count=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_OPERATION_COUNT) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "field_count=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_FIELD_COUNT) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "status_count=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT) != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "old_client_new_server=compatible-within-v1\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "new_client_old_server=certified\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "unknown_text_field_policy=ignored_by_schema_validation\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "required_field_policy=missing_required_field_fails_schema\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "oneof_policy=at_least_one_selector_field_required\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "idempotency_replay_match=operation-and-request-checksum\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "idempotency_conflict_status=version_conflict\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "idempotency_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "audit_log_persistence=store-journal-and-full-snapshot\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "journal_scope=completed-idempotency-and-audit-events\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "journal_truncation_policy=threshold-compaction\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "snapshot_full_restore_state=records-idempotency-audit\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "snapshot_paged_restore_state=records-only-clears-idempotency-audit\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_payload=put_object:v1-key-version-checksum-extra-field\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_payload=resolve_kv_segment:v1-key-or-block-hash-oneof\n") != 0 ||
        append_wire_schema_line(baseline,
                                baseline_len,
                                &used,
                                "baseline_payload=register_training_artifact:v1-training-step-compatible\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_compat_baseline_v1(void)
{
    char baseline[4096];
    size_t used = 0;

    if (render_compat_baseline_v1(baseline, sizeof(baseline), &used) != 0) {
        fprintf(stderr, "mem_service compat-baseline-v1: render failed\n");
        return 1;
    }
    (void)used;
    fputs(baseline, stdout);
    return 0;
}

static int run_compat_baseline_fixture_check(void)
{
    char baseline[4096];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;
    const struct mem_service_wire_operation_schema *put_object_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_PUT_OBJECT);
    const struct mem_service_wire_operation_schema *resolve_kv_schema =
        mem_service_wire_schema_for_operation(MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT);
    const struct mem_service_wire_operation_schema *training_publish_schema =
        mem_service_wire_schema_for_operation(
            MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT);
    struct mem_service_wire_payload_view old_put =
        mem_service_wire_payload_view_from_cstr(
            "key=old-client-object\n"
            "version=1\n"
            "checksum=2\n"
            "future_optional_field=ignored\n");
    struct mem_service_wire_payload_view old_put_missing_key =
        mem_service_wire_payload_view_from_cstr("version=1\nchecksum=2\n");
    struct mem_service_wire_payload_view old_resolve_kv =
        mem_service_wire_payload_view_from_cstr("block_hash=old-block\n");
    struct mem_service_wire_payload_view old_resolve_kv_missing_selector =
        mem_service_wire_payload_view_from_cstr("future_optional_field=ignored\n");
    struct mem_service_wire_payload_view old_training_publish =
        mem_service_wire_payload_view_from_cstr(
            "key=training/old/global-step-0001/commit\n"
            "session_id=old\n"
            "model_key=model-a\n"
            "artifact_kind=training-step-commit\n"
            "artifact_id=global-step-0001\n"
            "version=1\n"
            "checksum=101\n"
            "idempotency_key=old/global-step-0001/v1\n"
            "future_optional_field=ignored\n");

    if (render_compat_baseline_v1(baseline, sizeof(baseline), &used) != 0) {
        fprintf(stderr, "mem_service compat-baseline-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(baseline, used);
    if (used != MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: baseline len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: baseline checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (!mem_service_wire_schema_validate_payload(put_object_schema,
                                                  &old_put,
                                                  NULL) ||
        mem_service_wire_schema_validate_payload(put_object_schema,
                                                 &old_put_missing_key,
                                                 NULL) ||
        !mem_service_wire_schema_validate_payload(resolve_kv_schema,
                                                  &old_resolve_kv,
                                                  NULL) ||
        mem_service_wire_schema_validate_payload(resolve_kv_schema,
                                                 &old_resolve_kv_missing_selector,
                                                 NULL) ||
        !mem_service_wire_schema_validate_payload(training_publish_schema,
                                                  &old_training_publish,
                                                  NULL)) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: old payload policy failed\n");
        failures -= 1;
    }
    if (strstr(baseline, "old_client_new_server=compatible-within-v1\n") == NULL ||
        strstr(baseline, "new_client_old_server=certified\n") == NULL ||
        strstr(baseline,
               "idempotency_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(baseline,
               "audit_log_persistence=store-journal-and-full-snapshot\n") == NULL ||
        strstr(baseline,
               "baseline_payload=register_training_artifact:v1-training-step-compatible\n") ==
            NULL) {
        fprintf(stderr,
                "mem_service compat-baseline-fixtures: required baseline missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service compat-baseline-fixtures: status=ok baseline_version=1 "
           "baseline_len=%u baseline_checksum=0x%08x old_client_new_server=v1 "
           "new_client_old_server=certified\n",
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM);
    return 0;
}

static const struct mem_service_wire_payload_field *
find_schema_field(const struct mem_service_wire_operation_schema *schema,
                  const char *field_name)
{
    size_t i;

    if (schema == NULL || field_name == NULL) {
        return NULL;
    }
    for (i = 0; i < schema->field_count; ++i) {
        if (strcmp(schema->fields[i].name, field_name) == 0) {
            return &schema->fields[i];
        }
    }
    return NULL;
}

static int append_compat_profile_field(
    char *payload,
    size_t payload_len,
    const struct mem_service_wire_payload_field *field)
{
    if (field == NULL) {
        return -1;
    }
    if (field->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32 ||
        field->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
        return mem_service_wire_payload_append_u64(payload,
                                                   payload_len,
                                                   field->name,
                                                   1U);
    }
    return mem_service_wire_payload_append_field(payload,
                                                 payload_len,
                                                 field->name,
                                                 "compat");
}

static bool compat_profile_payload_has_field(const char *payload,
                                             const char *field_name)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload);
    char value[128];

    return mem_service_wire_payload_get_string(&view,
                                               field_name,
                                               value,
                                               sizeof(value));
}

static int render_compat_profile_payload(
    const struct mem_service_wire_operation_schema *schema,
    bool include_optional_fields,
    char *payload,
    size_t payload_len)
{
    size_t i;

    if (schema == NULL || payload == NULL || payload_len == 0) {
        return -1;
    }
    payload[0] = '\0';
    for (i = 0; i < schema->field_count; ++i) {
        const struct mem_service_wire_payload_field *field = &schema->fields[i];

        if ((field->required || include_optional_fields) &&
            append_compat_profile_field(payload, payload_len, field) != 0) {
            return -1;
        }
    }
    for (i = 0; i < schema->oneof_count; ++i) {
        const struct mem_service_wire_payload_oneof *oneof = &schema->oneofs[i];
        bool oneof_satisfied = false;
        size_t field_index;

        for (field_index = 0; field_index < oneof->field_count; ++field_index) {
            if (compat_profile_payload_has_field(payload,
                                                 oneof->field_names[field_index])) {
                oneof_satisfied = true;
                break;
            }
        }
        if (!oneof_satisfied && oneof->field_count > 0) {
            const struct mem_service_wire_payload_field *field =
                find_schema_field(schema, oneof->field_names[0]);

            if (append_compat_profile_field(payload, payload_len, field) != 0) {
                return -1;
            }
        }
    }
    if (include_optional_fields &&
        mem_service_wire_payload_append_field(payload,
                                              payload_len,
                                              "future_optional_field",
                                              "ignored") != 0) {
        return -1;
    }
    return 0;
}

static bool schema_has_required_field(
    const struct mem_service_wire_operation_schema *schema)
{
    size_t i;

    if (schema == NULL) {
        return false;
    }
    for (i = 0; i < schema->field_count; ++i) {
        if (schema->fields[i].required) {
            return true;
        }
    }
    return false;
}

static int render_compat_old_new_matrix(char *matrix,
                                        size_t matrix_len,
                                        size_t *used_out)
{
    size_t used = 0;
    size_t field_count = 0;
    size_t oneof_count = 0;
    size_t oneof_field_count = 0;

    if (matrix == NULL || matrix_len == 0) {
        return -1;
    }
    matrix[0] = '\0';
    wire_schema_count_fields(&field_count, &oneof_count, &oneof_field_count);
    if (append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "mem_service_old_new_compat_matrix_version=1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "matrix_name=mem-service-old-new-wire-v1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "matrix_scope=wire-header,schema-profile,payload-policy,response-status,release-artifact\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_old=1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_version_current=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_header_len_old=48\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_header_len_current=%u\n",
                                MEM_SERVICE_WIRE_HEADER_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_old=1\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_version_current=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_payload_format=text-kv\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_len=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_matrix_len=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_matrix_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_baseline_len=%u\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "compat_baseline_checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "operation_count=%zu\n",
                                wire_schema_operation_count()) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "field_count=%zu\n",
                                field_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "oneof_count=%zu\n",
                                oneof_count) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "status_count=%u\n",
                                MEM_SERVICE_COMPAT_MATRIX_STATUS_COUNT) != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "old_client_profile=v1-min-required-fields\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "current_client_profile=v1-current-fields-plus-future-optional\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "old_server_profile=v1-schema-validation-profile\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "current_server_profile=current-runtime-handlers\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "old_server_runtime_binary=in-tree\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-current-server:schema-compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-current-server:runtime-compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=current-client-old-schema-profile:schema-compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=current-client-current-server:wire-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=current-client-current-server:compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-missing-required:fail-closed\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=old-client-missing-oneof:fail-closed\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=unknown-text-field-forward:ignored\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=status-id-forward:stable\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "case=idempotency-forward:compatible\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certified_pair=old-v1-client->current-v1-server\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certified_pair=current-v1-client->old-v1-schema-profile\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certified_pair=current-v1-client->old-v1-runtime-binary\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=wire-schema-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=wire-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-baseline-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-old-new-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "evidence=compat-old-server-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "release_gate=install-smoke\n") != 0 ||
        append_wire_schema_line(matrix,
                                matrix_len,
                                &used,
                                "certification_limit=none\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_compat_old_new_matrix(void)
{
    char matrix[8192];
    size_t used = 0;

    if (render_compat_old_new_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-old-new-matrix: render failed\n");
        return 1;
    }
    (void)used;
    fputs(matrix, stdout);
    return 0;
}

static int run_compat_old_new_fixture_check(void)
{
    char matrix[8192];
    char old_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    char current_payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    struct mem_service_wire_payload_view view;
    size_t used = 0;
    size_t op_index;
    size_t old_payloads = 0;
    size_t current_payloads = 0;
    size_t required_fail_closed = 0;
    size_t oneof_fail_closed = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_compat_old_new_matrix(matrix, sizeof(matrix), &used) != 0) {
        fprintf(stderr, "mem_service compat-old-new-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(matrix, used);
    if (used != MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service compat-old-new-fixtures: matrix len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service compat-old-new-fixtures: matrix checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    for (op_index = 0; op_index < wire_schema_operation_count(); ++op_index) {
        const struct mem_service_wire_operation_schema *schema =
            &mem_service_wire_operation_schemas[op_index];

        if (render_compat_profile_payload(schema,
                                          false,
                                          old_payload,
                                          sizeof(old_payload)) != 0 ||
            render_compat_profile_payload(schema,
                                          true,
                                          current_payload,
                                          sizeof(current_payload)) != 0) {
            fprintf(stderr,
                    "mem_service compat-old-new-fixtures: payload render failed op=%s\n",
                    schema->name);
            failures -= 1;
            continue;
        }
        view = mem_service_wire_payload_view_from_cstr(old_payload);
        if (!mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
            fprintf(stderr,
                    "mem_service compat-old-new-fixtures: old payload rejected op=%s\n",
                    schema->name);
            failures -= 1;
        } else {
            old_payloads += 1;
        }
        view = mem_service_wire_payload_view_from_cstr(current_payload);
        if (!mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
            fprintf(stderr,
                    "mem_service compat-old-new-fixtures: current payload rejected op=%s\n",
                    schema->name);
            failures -= 1;
        } else {
            current_payloads += 1;
        }
        if (schema_has_required_field(schema)) {
            view = mem_service_wire_payload_view_from_cstr(
                "future_optional_field=ignored\n");
            if (mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
                fprintf(stderr,
                        "mem_service compat-old-new-fixtures: missing required accepted op=%s\n",
                        schema->name);
                failures -= 1;
            } else {
                required_fail_closed += 1;
            }
        }
        if (schema->oneof_count > 0) {
            view = mem_service_wire_payload_view_from_cstr(
                "future_optional_field=ignored\n");
            if (mem_service_wire_schema_validate_payload(schema, &view, NULL)) {
                fprintf(stderr,
                        "mem_service compat-old-new-fixtures: missing oneof accepted op=%s\n",
                        schema->name);
                failures -= 1;
            } else {
                oneof_fail_closed += 1;
            }
        }
    }
    if (old_payloads != wire_schema_operation_count() ||
        current_payloads != wire_schema_operation_count() ||
        MEM_SERVICE_WIRE_VERSION != 1U ||
        MEM_SERVICE_WIRE_HEADER_LEN != 48U ||
        MEM_SERVICE_WIRE_SCHEMA_VERSION != 1U ||
        MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT != 4U ||
        MEM_SERVICE_WIRE_STATUS_UNSUPPORTED != 9U) {
        fprintf(stderr, "mem_service compat-old-new-fixtures: version matrix failed\n");
        failures -= 1;
    }
    if (strstr(matrix, "old_server_runtime_binary=in-tree\n") == NULL ||
        strstr(matrix,
               "certified_pair=current-v1-client->old-v1-schema-profile\n") == NULL ||
        strstr(matrix,
               "case=old-client-current-server:runtime-compatible\n") == NULL ||
        strstr(matrix,
               "certified_pair=current-v1-client->old-v1-runtime-binary\n") ==
            NULL ||
        strstr(matrix, "evidence=compat-old-new-fixtures\n") == NULL ||
        strstr(matrix, "evidence=compat-runtime-fixtures\n") == NULL ||
        strstr(matrix, "evidence=compat-old-server-runtime-fixtures\n") == NULL) {
        fprintf(stderr,
                "mem_service compat-old-new-fixtures: required matrix rule missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service compat-old-new-fixtures: status=ok matrix_len=%u "
           "matrix_checksum=0x%08x old_payloads=%zu current_payloads=%zu "
           "required_fail_closed=%zu oneof_fail_closed=%zu "
           "old_server_runtime_binary=in-tree\n",
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM,
           old_payloads,
           current_payloads,
           required_fail_closed,
           oneof_fail_closed);
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

static int render_version_manifest(char *manifest,
                                   size_t manifest_len,
                                   size_t *used_out)
{
    size_t used = 0;

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_version=%s\n",
                                MEM_SERVICE_RELEASE_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_contract_version=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "version_contract=text-kv\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_version=%u\n",
                                MEM_SERVICE_WIRE_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_manifest_version=%u\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "wire_schema_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "api_abi_policy_version=%u\n",
                                MEM_SERVICE_API_ABI_POLICY_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "api_abi_policy_checksum=0x%08x\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_version=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_len=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_manifest_command=release-manifest\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_manifest_command=package-manifest\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "config_security_gate=config-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "version_gate=version-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "ops_certification_status=not-certified-until-external-evidence\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_version_manifest(void)
{
    char manifest[2048];
    size_t used = 0;

    if (render_version_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service version: render failed\n");
        return 1;
    }
    fwrite(manifest, 1, used, stdout);
    return 0;
}

static int run_version_fixture_check(void)
{
    char manifest[2048];
    size_t used = 0;

    if (render_version_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service version-fixtures: render failed\n");
        return 1;
    }
    if (strstr(manifest, "service_name=linqu_mem_service\n") == NULL ||
        strstr(manifest, "service_version=" MEM_SERVICE_RELEASE_VERSION "\n") == NULL ||
        strstr(manifest, "version_contract=text-kv\n") == NULL ||
        strstr(manifest, "wire_version=1\n") == NULL ||
        strstr(manifest, "wire_schema_manifest_checksum=0xf4cf34c6\n") == NULL ||
        strstr(manifest, "api_abi_policy_checksum=0x5d95ae02\n") == NULL ||
        strstr(manifest, "package_manifest_checksum=0x") == NULL ||
        strstr(manifest, "release_manifest_command=release-manifest\n") == NULL ||
        strstr(manifest, "package_manifest_command=package-manifest\n") == NULL ||
        strstr(manifest, "config_security_gate=config-fixtures\n") == NULL ||
        strstr(manifest, "version_gate=version-fixtures\n") == NULL) {
        fprintf(stderr, "mem_service version-fixtures: required field missing\n");
        return 1;
    }
    printf("mem_service version-fixtures: status=ok service_version=%s "
           "wire_version=%u package_manifest_len=%u "
           "package_manifest_checksum=0x%08x\n",
           MEM_SERVICE_RELEASE_VERSION,
           MEM_SERVICE_WIRE_VERSION,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
    (void)used;
    return 0;
}

static int render_package_manifest(char *manifest,
                                   size_t manifest_len,
                                   size_t *used_out)
{
    size_t used = 0;

    if (manifest == NULL || manifest_len == 0) {
        return -1;
    }
    manifest[0] = '\0';
    if (append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "mem_service_package_manifest_version=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_VERSION) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_format=installed-layout-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_format=tar\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_name=%s\n",
                                MEM_SERVICE_PACKAGE_TARBALL_NAME) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_root=usr+etc\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_install_prefix=/usr\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_contents=installed-layout-v1-root\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "artifact_gate=package-tarball-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_format=deb\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_name=%s\n",
                                MEM_SERVICE_NATIVE_DEB_NAME) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_arch=arm64\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_payload=debian-binary+control.tar.gz+data.tar.gz\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_gate=package-deb-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "native_package_runtime=not-executed-cross-compiled-arm64\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_format=rpm\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_name=%s\n",
                                MEM_SERVICE_NATIVE_RPM_NAME) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_arch=aarch64\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_payload=rpm-cpio+metadata\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_gate=package-rpm-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "rpm_package_runtime=requires-linux-rpm-toolchain\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "package_scope=core-daemon+host-daemon+client-sdk+examples+contracts+deploy+runtime-config+systemd-units+release-scripts\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "prefix_default=/usr\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary=bin/linqu_mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_binary=libexec/lingqu/mem_service/linqu_mem_service_host\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary_version_command=version\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary_version_contract=text-kv\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "binary_version_gate=version-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "optional_adapter=bin/linqu_mem_service_qwen3\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "default_endpoint=%s\n",
                                mem_service_default_unix_socket_spec()) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "data_root=share/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "header_root=include/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "source_root=src/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig=lib/pkgconfig/lingqu-mem-service.pc\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_name=lingqu-mem-service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_cflags=-I${includedir}\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_example_smoke=installed-sdk-example-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_example_smoke_scope=serving+pretraining-external-client-compile\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "config_root=share/lingqu/mem_service/config\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "system_config_root=etc/lingqu/mem_service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "runtime_config=etc/lingqu/mem_service/mem_service.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "service_auth_boundary=unix-socket-local-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "metrics_auth_boundary=loopback-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "config_security_gate=config-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "deploy_root=share/lingqu/mem_service/deploy\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "systemd_unit_root=lib/systemd/system\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "systemd_unit=lib/systemd/system/linqu_mem_service.service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script_root=share/lingqu/mem_service/scripts\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_linux_ops_evidence.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_evidence.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "installed_file_count=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=core_binary count=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=host_binary count=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=public_headers count=8\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=client_sources count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=pkgconfig count=1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=examples count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=contracts count=10\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=configs count=4\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=runtime_config count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=deploy count=3\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=systemd_units count=2\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "file_class=release_scripts count=9\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=release-manifest path=share/lingqu/mem_service/release-manifest.txt\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=wire-schema path=share/lingqu/mem_service/wire-schema.txt checksum=0x%08x\n",
                                MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=admin-output-schema path=share/lingqu/mem_service/admin-output-schema.txt checksum=0x%08x\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=upgrade-rollback-policy path=share/lingqu/mem_service/upgrade-rollback-policy.txt checksum=0x%08x\n",
                                MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=api-abi-policy path=share/lingqu/mem_service/api-abi-policy.txt checksum=0x%08x\n",
                                MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=compat-matrix path=share/lingqu/mem_service/compat-matrix.txt checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=compat-baseline-v1 path=share/lingqu/mem_service/compat-baseline-v1.txt checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=compat-old-new-matrix path=share/lingqu/mem_service/compat-old-new-matrix.txt checksum=0x%08x\n",
                                MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=alert-rules path=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml checksum=0x%08x\n",
                                MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "contract=ops-certification-policy path=share/lingqu/mem_service/ops-certification-policy.txt checksum=0x%08x\n",
                                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate_count=%u\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT) != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=release-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=version-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=admin-output-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=upgrade-rollback-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=upgrade-rollback-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=restore-policy-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=api-abi-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=compat-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=compat-runtime-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=collector-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=alert-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=alert-integration-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=ops-certification-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=ops-certification-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=remote-transport-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=ops-certification-linux-ci-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=durable-catalog-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=host-artifact-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-tarball-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-deb-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=package-rpm-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=install-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=installed-sdk-example-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "required_gate=installed-sdk-runtime-smoke\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "serving_api=typed-c-client-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "pretraining_api=typed-c-client-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_ownership_matrix=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_ownership_scope=artifact-query-expected-owner\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "restore_policy=transactional-staged-restore\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "restore_policy_gate=restore-policy-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "durable_backend=snapshot+journal\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_network_transport=not-certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_generate=remote-transport-generate-evidence\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_verify=remote-transport-verify --evidence-file\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "upgrade_policy=current-version-only\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "old_server_runtime_binary=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "cross_version_upgrade=certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "real_systemd_environment=not-certified\n") != 0 ||
        append_wire_schema_line(manifest,
                                manifest_len,
                                &used,
                                "production_collector_alert_environment=not-certified\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_package_manifest(void)
{
    char manifest[8192];
    size_t used = 0;

    if (render_package_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service package-manifest: render failed\n");
        return 1;
    }
    fwrite(manifest, 1, used, stdout);
    return 0;
}

static int render_ops_certification_policy(char *policy,
                                           size_t policy_len,
                                           size_t *used_out)
{
    size_t used = 0;

    if (policy == NULL || policy_len == 0) {
        return -1;
    }
    if (append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "mem_service_ops_certification_policy_version=%u\n",
                                MEM_SERVICE_OPS_CERTIFICATION_POLICY_VERSION) != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "certification_scope=real-linux-operations\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "certification_status=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "admission_rule=fail-closed-until-external-evidence\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_schema=ops-certification-evidence-v1\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_verify=ops-certification-verify --evidence-file\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_generate=ops-certification-generate-evidence\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_ci_gate=ops-certification-linux-ci-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "evidence_gate=ops-certification-evidence-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "local_gate=deployment-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "local_gate=collector-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "local_gate=alert-integration-fixtures\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=linux-systemd-service-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=linux-systemd-host-service-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=prometheus-scrape-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=prometheus-alertmanager-rule-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=rpm-package-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "external_gate=upgrade-rollback-deployment-smoke\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_environment=os=linux\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_environment=init=systemd\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=systemctl\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=journalctl\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=promtool\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=rpmbuild\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "required_tool=rpm2cpio\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "real_systemd_environment=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "production_collector_alert_environment=not-certified\n") != 0 ||
        append_wire_schema_line(policy,
                                policy_len,
                                &used,
                                "rpm_package=not-certified\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_ops_certification_policy(void)
{
    char policy[4096];
    size_t used = 0;

    if (render_ops_certification_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service ops-certification-policy: render failed\n");
        return 1;
    }
    fwrite(policy, 1, used, stdout);
    return 0;
}

static int run_ops_certification_fixture_check(void)
{
    char policy[4096];
    size_t used = 0;
    uint32_t checksum;

    if (render_ops_certification_policy(policy, sizeof(policy), &used) != 0) {
        fprintf(stderr, "mem_service ops-certification-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(policy, used);
    if (used != MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service ops-certification-fixtures: policy len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN);
        return 1;
    }
    if (checksum != MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service ops-certification-fixtures: policy checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM);
        return 1;
    }
    if (strstr(policy, "certification_status=not-certified\n") == NULL ||
        strstr(policy,
               "admission_rule=fail-closed-until-external-evidence\n") == NULL ||
        strstr(policy, "evidence_schema=ops-certification-evidence-v1\n") == NULL ||
        strstr(policy,
               "evidence_gate=ops-certification-evidence-fixtures\n") == NULL ||
        strstr(policy, "external_gate=linux-systemd-service-smoke\n") == NULL ||
        strstr(policy,
               "external_gate=prometheus-alertmanager-rule-smoke\n") == NULL ||
        strstr(policy, "external_gate=rpm-package-smoke\n") == NULL ||
        strstr(policy, "required_environment=init=systemd\n") == NULL ||
        strstr(policy, "real_systemd_environment=not-certified\n") == NULL ||
        strstr(policy,
               "production_collector_alert_environment=not-certified\n") == NULL) {
        fprintf(stderr,
                "mem_service ops-certification-fixtures: required policy missing\n");
        return 1;
    }
    printf("mem_service ops-certification-fixtures: status=ok policy_version=%u "
           "policy_len=%u policy_checksum=0x%08x "
           "certification_status=not-certified external_gates=6 "
           "admission_rule=fail-closed-until-external-evidence\n",
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_VERSION,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM);
    return 0;
}

static bool mem_service_payload_string_equals(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    const char *expected)
{
    char value[160];

    return mem_service_wire_payload_get_string(view, name, value, sizeof(value)) &&
           strcmp(value, expected) == 0;
}

static int validate_ops_certification_evidence(const char *evidence,
                                               char *reason,
                                               size_t reason_len)
{
    struct mem_service_wire_payload_view view;
    uint64_t policy_checksum;
    uint64_t package_checksum;
    uint32_t version;
    static const char *required_pass_gates[] = {
        "linux_systemd_service_smoke",
        "linux_systemd_host_service_smoke",
        "prometheus_scrape_smoke",
        "prometheus_alertmanager_rule_smoke",
        "rpm_package_smoke",
        "upgrade_rollback_deployment_smoke",
    };
    size_t i;

    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }
    if (evidence == NULL || evidence[0] == '\0') {
        snprintf(reason, reason_len, "empty-evidence");
        return -1;
    }
    view = mem_service_wire_payload_view_from_cstr(evidence);
    version = mem_service_wire_payload_get_u32(
        &view, "mem_service_ops_certification_evidence_version", 0);
    if (version != MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION) {
        snprintf(reason, reason_len, "bad-evidence-version");
        return -1;
    }
    if (!mem_service_payload_string_equals(&view,
                                           "service_name",
                                           "linqu_mem_service") ||
        !mem_service_payload_string_equals(&view,
                                           "certification_scope",
                                           "real-linux-operations") ||
        !mem_service_payload_string_equals(&view, "evidence_os", "linux") ||
        !mem_service_payload_string_equals(&view, "evidence_init", "systemd")) {
        snprintf(reason, reason_len, "bad-evidence-identity");
        return -1;
    }
    if (!mem_service_wire_payload_get_u64_checked(&view,
                                                   "ops_certification_policy_checksum",
                                                   &policy_checksum) ||
        policy_checksum != MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) {
        snprintf(reason, reason_len, "bad-policy-checksum");
        return -1;
    }
    if (!mem_service_wire_payload_get_u64_checked(&view,
                                                   "package_manifest_checksum",
                                                   &package_checksum) ||
        package_checksum != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) {
        snprintf(reason, reason_len, "bad-package-checksum");
        return -1;
    }
    for (i = 0; i < sizeof(required_pass_gates) / sizeof(required_pass_gates[0]); ++i) {
        if (!mem_service_payload_string_equals(&view,
                                               required_pass_gates[i],
                                               "pass")) {
            snprintf(reason, reason_len, "gate-not-pass:%s", required_pass_gates[i]);
            return -1;
        }
    }
    return 0;
}

static int validate_remote_transport_evidence(const char *evidence,
                                              char *reason,
                                              size_t reason_len)
{
    struct mem_service_wire_payload_view view;
    uint64_t package_checksum;
    uint32_t version;
    static const char *required_pass_gates[] = {
        "source_address_non_loopback",
        "payload_block_round_trip",
        "payload_checksum_validation",
        "payload_corruption_fail_closed",
        "producer_consumer_distinct_hosts",
        "network_partition_fail_closed",
    };
    size_t i;

    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }
    if (evidence == NULL || evidence[0] == '\0') {
        snprintf(reason, reason_len, "empty-evidence");
        return -1;
    }
    view = mem_service_wire_payload_view_from_cstr(evidence);
    version = mem_service_wire_payload_get_u32(
        &view, "mem_service_remote_transport_evidence_version", 0);
    if (version != MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION) {
        snprintf(reason, reason_len, "bad-evidence-version");
        return -1;
    }
    if (!mem_service_payload_string_equals(&view,
                                           "service_name",
                                           "linqu_mem_service") ||
        !mem_service_payload_string_equals(&view,
                                           "certification_scope",
                                           "production-network-transport") ||
        !mem_service_payload_string_equals(&view,
                                           "transport_backend",
                                           "transport-tcp-block-v1") ||
        !mem_service_payload_string_equals(&view, "transport_protocol", "tcp-ipv4") ||
        !mem_service_payload_string_equals(&view, "transport_topology", "cross-host")) {
        snprintf(reason, reason_len, "bad-evidence-identity");
        return -1;
    }
    if (!mem_service_wire_payload_get_u64_checked(&view,
                                                   "package_manifest_checksum",
                                                   &package_checksum) ||
        package_checksum != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) {
        snprintf(reason, reason_len, "bad-package-checksum");
        return -1;
    }
    for (i = 0; i < sizeof(required_pass_gates) / sizeof(required_pass_gates[0]); ++i) {
        if (!mem_service_payload_string_equals(&view,
                                               required_pass_gates[i],
                                               "pass")) {
            snprintf(reason, reason_len, "gate-not-pass:%s", required_pass_gates[i]);
            return -1;
        }
    }
    return 0;
}

static int read_text_file_limited(const char *path, char *payload, size_t payload_len)
{
    FILE *file;
    size_t used;

    if (path == NULL || path[0] == '\0' || payload == NULL || payload_len == 0) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    used = fread(payload, 1, payload_len - 1U, file);
    if (ferror(file) != 0 || (!feof(file) && used == payload_len - 1U)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    payload[used] = '\0';
    return 0;
}

static int write_text_file_limited(const char *path,
                                   const char *payload,
                                   size_t payload_len)
{
    FILE *file;

    if (path == NULL || path[0] == '\0' || payload == NULL) {
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, file) != payload_len) {
        fclose(file);
        return -1;
    }
    if (fflush(file) != 0) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static bool ops_certification_command_ok(const char *command)
{
    int rc;

    if (command == NULL || command[0] == '\0') {
        return false;
    }
    rc = system(command);
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static bool ops_certification_command_exists(const char *name)
{
    char command[160];

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (snprintf(command,
                 sizeof(command),
                 "command -v %s >/dev/null 2>&1",
                 name) >= (int)sizeof(command)) {
        return false;
    }
    return ops_certification_command_ok(command);
}

static bool ops_certification_safe_path(const char *path)
{
    size_t i;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    for (i = 0; path[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)path[i];

        if (!(isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-' ||
              ch == ':' || ch == '+')) {
            return false;
        }
    }
    return true;
}

static bool ops_certification_host_is_linux(void)
{
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

static bool ops_certification_systemd_is_available(void)
{
    return access("/run/systemd/system", F_OK) == 0 &&
           ops_certification_command_exists("systemctl") &&
           ops_certification_command_exists("journalctl");
}

static bool ops_certification_service_is_active(const char *unit)
{
    char command[192];

    if (unit == NULL || unit[0] == '\0') {
        return false;
    }
    if (snprintf(command,
                 sizeof(command),
                 "systemctl is-active --quiet %s",
                 unit) >= (int)sizeof(command)) {
        return false;
    }
    return ops_certification_command_ok(command);
}

static bool ops_certification_metrics_scrape_passes(void)
{
    FILE *pipe;
    char line[256];
    bool found = false;

    if (!ops_certification_command_exists("curl")) {
        return false;
    }
    pipe = popen("curl -fsS http://127.0.0.1:9900/metrics 2>/dev/null", "r");
    if (pipe == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), pipe) != NULL) {
        if (strstr(line, "lingqu_mem_service_") != NULL) {
            found = true;
            break;
        }
    }
    if (pclose(pipe) == -1) {
        return false;
    }
    return found;
}

static bool ops_certification_alert_rules_pass(void)
{
    return ops_certification_command_exists("promtool") &&
           ops_certification_command_ok(
               "promtool check rules "
               "/usr/share/lingqu/mem_service/deploy/"
               "linqu_mem_service.prometheus-alerts.yml >/dev/null 2>&1");
}

static bool ops_certification_rpm_package_pass(const char *rpm_file)
{
    char command[512];

    if (!ops_certification_safe_path(rpm_file) ||
        !ops_certification_command_exists("rpmbuild") ||
        !ops_certification_command_exists("rpm2cpio") ||
        access(rpm_file, R_OK) != 0) {
        return false;
    }
    if (snprintf(command,
                 sizeof(command),
                 "rpm2cpio %s >/dev/null 2>&1",
                 rpm_file) >= (int)sizeof(command)) {
        return false;
    }
    return ops_certification_command_ok(command);
}

static bool ops_certification_upgrade_rollback_pass(const char *marker_path)
{
    char marker[512];

    if (!ops_certification_safe_path(marker_path) ||
        read_text_file_limited(marker_path, marker, sizeof(marker)) != 0) {
        return false;
    }
    return strstr(marker, "upgrade_rollback_deployment_smoke=pass\n") != NULL;
}

static int append_ops_certification_gate_status(char *evidence,
                                                size_t evidence_len,
                                                size_t *used,
                                                const char *name,
                                                bool passed)
{
    return append_wire_schema_line(evidence,
                                   evidence_len,
                                   used,
                                   "%s=%s\n",
                                   name,
                                   passed ? "pass" : "fail");
}

static int render_ops_certification_generated_evidence(int argc,
                                                       char **argv,
                                                       char *evidence,
                                                       size_t evidence_len,
                                                       size_t *used_out)
{
    const char *rpm_file = option_value(argc, argv, "--rpm-file");
    const char *upgrade_marker = option_value(argc, argv, "--upgrade-rollback-marker");
    size_t used = 0;
    bool linux_host = ops_certification_host_is_linux();
    bool systemd_host = linux_host && ops_certification_systemd_is_available();
    bool service_pass = systemd_host &&
                        ops_certification_service_is_active(
                            "linqu_mem_service.service");
    bool host_service_pass = systemd_host &&
                             ops_certification_service_is_active(
                                 "linqu_mem_service.host.service");
    bool scrape_pass = linux_host && ops_certification_metrics_scrape_passes();
    bool alert_pass = linux_host && ops_certification_alert_rules_pass();
    bool rpm_pass = linux_host && ops_certification_rpm_package_pass(rpm_file);
    bool upgrade_pass = linux_host &&
                        ops_certification_upgrade_rollback_pass(upgrade_marker);

    if (evidence == NULL || evidence_len == 0) {
        return -1;
    }
    evidence[0] = '\0';
    if (append_wire_schema_line(
            evidence,
            evidence_len,
            &used,
            "mem_service_ops_certification_evidence_version=%u\n",
            MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "certification_scope=real-linux-operations\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_os=%s\n",
                                linux_host ? "linux" : "non-linux") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_init=%s\n",
                                systemd_host ? "systemd" : "not-systemd") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_generator=ops-certification-generate-evidence\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "ops_certification_policy_checksum=0x%08x\n",
                                MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "linux_systemd_service_smoke",
                                             service_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "linux_systemd_host_service_smoke",
                                             host_service_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "prometheus_scrape_smoke",
                                             scrape_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "prometheus_alertmanager_rule_smoke",
                                             alert_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "rpm_package_smoke",
                                             rpm_pass) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "upgrade_rollback_deployment_smoke",
                                             upgrade_pass) != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_ops_certification_generate_evidence(int argc, char **argv)
{
    char evidence[4096];
    size_t used = 0;

    if (render_ops_certification_generated_evidence(
            argc, argv, evidence, sizeof(evidence), &used) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-generate-evidence: render failed\n");
        return 1;
    }
    fwrite(evidence, 1, used, stdout);
    return 0;
}

static int run_ops_certification_linux_ci_smoke(int argc, char **argv)
{
    const char *evidence_file = option_value(argc, argv, "--evidence-file");
    char evidence[4096];
    char reason[160];
    size_t used = 0;

    if (evidence_file == NULL || evidence_file[0] == '\0') {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: missing --evidence-file\n");
        return 2;
    }
    if (!ops_certification_safe_path(evidence_file)) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: unsafe evidence file\n");
        return 2;
    }
    if (render_ops_certification_generated_evidence(
            argc, argv, evidence, sizeof(evidence), &used) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: render failed\n");
        return 1;
    }
    if (write_text_file_limited(evidence_file, evidence, used) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: evidence write failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-linux-ci-smoke: fail-closed "
                "reason=%s evidence_file=%s\n",
                reason,
                evidence_file);
        return 1;
    }
    printf("mem_service ops-certification-linux-ci-smoke: status=ok "
           "certification_status=certified evidence_file=%s external_gates=6\n",
           evidence_file);
    return 0;
}

static int run_ops_certification_verify(int argc, char **argv)
{
    const char *path = option_value(argc, argv, "--evidence-file");
    char evidence[4096];
    char reason[160];

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "mem_service ops-certification-verify: missing --evidence-file\n");
        return 2;
    }
    if (read_text_file_limited(path, evidence, sizeof(evidence)) != 0) {
        fprintf(stderr, "mem_service ops-certification-verify: evidence read failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-verify: fail-closed reason=%s\n",
                reason);
        return 1;
    }
    printf("mem_service ops-certification-verify: status=ok "
           "certification_status=certified evidence_version=%u external_gates=6\n",
           MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION);
    return 0;
}

static int run_remote_transport_verify(int argc, char **argv)
{
    const char *path = option_value(argc, argv, "--evidence-file");
    char evidence[2048];
    char reason[160];

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "mem_service remote-transport-verify: missing --evidence-file\n");
        return 2;
    }
    if (read_text_file_limited(path, evidence, sizeof(evidence)) != 0) {
        fprintf(stderr, "mem_service remote-transport-verify: evidence read failed\n");
        return 1;
    }
    if (validate_remote_transport_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-verify: fail-closed reason=%s\n",
                reason);
        return 1;
    }
    printf("mem_service remote-transport-verify: status=ok "
           "certification_status=certified evidence_version=%u external_gates=6\n",
           MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION);
    return 0;
}

static bool remote_transport_parse_source_ip(const char *source,
                                             char *ip,
                                             size_t ip_len)
{
    const char *start;
    const char *port;
    size_t len;

    if (source == NULL || strncmp(source, "tcp:", 4) != 0 ||
        ip == NULL || ip_len == 0) {
        return false;
    }
    start = source + 4;
    port = strrchr(start, ':');
    if (port == NULL || port == start || port[1] == '\0') {
        return false;
    }
    len = (size_t)(port - start);
    if (len == 0 || len >= ip_len) {
        return false;
    }
    memcpy(ip, start, len);
    ip[len] = '\0';
    return true;
}

static bool remote_transport_source_ip_is_non_loopback(const char *source)
{
    char ip[80];

    if (!remote_transport_parse_source_ip(source, ip, sizeof(ip))) {
        return false;
    }
    if (strcmp(ip, "0.0.0.0") == 0 ||
        strcmp(ip, "127.0.0.1") == 0 ||
        strcmp(ip, "localhost") == 0 ||
        strncmp(ip, "127.", 4) == 0) {
        return false;
    }
    return true;
}

static bool remote_transport_hosts_are_distinct(const char *producer_host,
                                                const char *consumer_host)
{
    return producer_host != NULL && producer_host[0] != '\0' &&
           consumer_host != NULL && consumer_host[0] != '\0' &&
           strcmp(producer_host, consumer_host) != 0;
}

static bool remote_transport_partition_marker_passes(const char *marker_path)
{
    char marker[512];

    if (!ops_certification_safe_path(marker_path) ||
        read_text_file_limited(marker_path, marker, sizeof(marker)) != 0) {
        return false;
    }
    return strstr(marker, "network_partition_fail_closed=pass\n") != NULL;
}

static int render_remote_transport_generated_evidence(
    const char *source,
    const char *producer_host,
    const char *consumer_host,
    const char *partition_marker,
    const struct mem_service_remote_transport_probe_result *probe,
    char *evidence,
    size_t evidence_len,
    size_t *used_out)
{
    size_t used = 0;
    bool source_non_loopback = remote_transport_source_ip_is_non_loopback(source);
    bool distinct_hosts =
        remote_transport_hosts_are_distinct(producer_host, consumer_host);
    bool partition_pass =
        remote_transport_partition_marker_passes(partition_marker);

    if (probe == NULL || evidence == NULL || evidence_len == 0) {
        return -1;
    }
    evidence[0] = '\0';
    if (append_wire_schema_line(
            evidence,
            evidence_len,
            &used,
            "mem_service_remote_transport_evidence_version=%u\n",
            MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "certification_scope=production-network-transport\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "evidence_generator=remote-transport-generate-evidence\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "transport_backend=transport-tcp-block-v1\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "transport_protocol=tcp-ipv4\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "transport_topology=cross-host\n") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "producer_host=%s\n",
                                producer_host != NULL ? producer_host : "") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "consumer_host=%s\n",
                                consumer_host != NULL ? consumer_host : "") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "source=%s\n",
                                source != NULL ? source : "") != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "package_manifest_checksum=0x%08x\n",
                                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "payload_len=%" PRIu64 "\n",
                                probe->payload_len) != 0 ||
        append_wire_schema_line(evidence,
                                evidence_len,
                                &used,
                                "payload_checksum=0x%016" PRIx64 "\n",
                                probe->payload_checksum) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "source_address_non_loopback",
                                             source_non_loopback) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "payload_block_round_trip",
                                             probe->payload_block_round_trip) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "payload_checksum_validation",
                                             probe->payload_checksum_validation) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "payload_corruption_fail_closed",
                                             probe->payload_corruption_fail_closed) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "producer_consumer_distinct_hosts",
                                             distinct_hosts) != 0 ||
        append_ops_certification_gate_status(evidence,
                                             evidence_len,
                                             &used,
                                             "network_partition_fail_closed",
                                             partition_pass) != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_remote_transport_generate_evidence(int argc, char **argv)
{
    const char *source = option_value(argc, argv, "--source");
    const char *producer_host = option_value(argc, argv, "--producer-host");
    const char *consumer_host = option_value(argc, argv, "--consumer-host");
    const char *partition_marker =
        option_value(argc, argv, "--network-partition-marker");
    const char *evidence_file = option_value(argc, argv, "--evidence-file");
    const char *storage_root_arg = option_value(argc, argv, "--storage-root");
    char storage_root[256];
    char evidence[2048];
    char reason[160];
    size_t used = 0;
    struct mem_service_remote_transport_probe_result probe;

    if (source == NULL || producer_host == NULL || consumer_host == NULL ||
        partition_marker == NULL || evidence_file == NULL ||
        source[0] == '\0' || producer_host[0] == '\0' ||
        consumer_host[0] == '\0' || partition_marker[0] == '\0' ||
        evidence_file[0] == '\0') {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: missing required option\n");
        return 2;
    }
    if (!ops_certification_safe_path(evidence_file) ||
        !ops_certification_safe_path(partition_marker)) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: unsafe evidence path\n");
        return 2;
    }
    if (storage_root_arg != NULL && storage_root_arg[0] != '\0') {
        if (!ops_certification_safe_path(storage_root_arg) ||
            strlen(storage_root_arg) >= sizeof(storage_root)) {
            fprintf(stderr,
                    "mem_service remote-transport-generate-evidence: unsafe storage root\n");
            return 2;
        }
        strcpy(storage_root, storage_root_arg);
    } else if (snprintf(storage_root,
                        sizeof(storage_root),
                        "/tmp/linqu_mem_service_remote_transport_probe_%ld",
                        (long)getpid()) >= (int)sizeof(storage_root)) {
        return 1;
    }
    if (mem_service_probe_transport_tcp_payload_block(storage_root,
                                                      source,
                                                      &probe) != 0) {
        memset(&probe, 0, sizeof(probe));
    }
    if (render_remote_transport_generated_evidence(source,
                                                   producer_host,
                                                   consumer_host,
                                                   partition_marker,
                                                   &probe,
                                                   evidence,
                                                   sizeof(evidence),
                                                   &used) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: render failed\n");
        return 1;
    }
    if (write_text_file_limited(evidence_file, evidence, used) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: evidence write failed\n");
        return 1;
    }
    if (validate_remote_transport_evidence(evidence, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-generate-evidence: fail-closed "
                "reason=%s evidence_file=%s\n",
                reason,
                evidence_file);
        return 1;
    }
    printf("mem_service remote-transport-generate-evidence: status=ok "
           "certification_status=certified evidence_file=%s external_gates=6 "
           "payload_len=%" PRIu64 "\n",
           evidence_file,
           probe.payload_len);
    return 0;
}

static int append_ops_certification_valid_evidence(char *evidence,
                                                   size_t evidence_len)
{
    size_t used = 0;

    evidence[0] = '\0';
    return append_wire_schema_line(
               evidence,
               evidence_len,
               &used,
               "mem_service_ops_certification_evidence_version=%u\n",
               MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "service_name=linqu_mem_service\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "certification_scope=real-linux-operations\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "evidence_os=linux\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "evidence_init=systemd\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "ops_certification_policy_checksum=0x%08x\n",
                                   MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "package_manifest_checksum=0x%08x\n",
                                   MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "linux_systemd_service_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "linux_systemd_host_service_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "prometheus_scrape_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "prometheus_alertmanager_rule_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "rpm_package_smoke=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "upgrade_rollback_deployment_smoke=pass\n") != 0
               ? -1
               : 0;
}

static int append_remote_transport_valid_evidence(char *evidence,
                                                  size_t evidence_len)
{
    size_t used = 0;

    evidence[0] = '\0';
    return append_wire_schema_line(
               evidence,
               evidence_len,
               &used,
               "mem_service_remote_transport_evidence_version=%u\n",
               MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "service_name=linqu_mem_service\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "certification_scope=production-network-transport\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "transport_backend=transport-tcp-block-v1\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "transport_protocol=tcp-ipv4\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "transport_topology=cross-host\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "package_manifest_checksum=0x%08x\n",
                                   MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "source_address_non_loopback=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "payload_block_round_trip=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "payload_checksum_validation=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "payload_corruption_fail_closed=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "producer_consumer_distinct_hosts=pass\n") != 0 ||
           append_wire_schema_line(evidence,
                                   evidence_len,
                                   &used,
                                   "network_partition_fail_closed=pass\n") != 0
               ? -1
               : 0;
}

static int run_ops_certification_evidence_fixture_check(void)
{
    char valid[2048];
    char bad_gate[2048];
    char bad_checksum[2048];
    char reason[160];

    if (append_ops_certification_valid_evidence(valid, sizeof(valid)) != 0) {
        fprintf(stderr, "mem_service ops-certification-evidence-fixtures: render failed\n");
        return 1;
    }
    if (validate_ops_certification_evidence(valid, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service ops-certification-evidence-fixtures: valid rejected reason=%s\n",
                reason);
        return 1;
    }
    strcpy(bad_gate, valid);
    {
        char *gate = strstr(bad_gate, "rpm_package_smoke=pass\n");

        if (gate == NULL) {
            return 1;
        }
        memcpy(gate, "rpm_package_smoke=fail\n", strlen("rpm_package_smoke=fail\n"));
    }
    if (validate_ops_certification_evidence(bad_gate, reason, sizeof(reason)) == 0 ||
        strstr(reason, "rpm_package_smoke") == NULL) {
        fprintf(stderr,
                "mem_service ops-certification-evidence-fixtures: bad gate accepted\n");
        return 1;
    }
    strcpy(bad_checksum, valid);
    {
        char *checksum = strstr(bad_checksum, "package_manifest_checksum=0x");

        if (checksum == NULL) {
            return 1;
        }
        memcpy(checksum,
               "package_manifest_checksum=0x00000000",
               strlen("package_manifest_checksum=0x00000000"));
    }
    if (validate_ops_certification_evidence(bad_checksum, reason, sizeof(reason)) == 0 ||
        strcmp(reason, "bad-package-checksum") != 0) {
        fprintf(stderr,
                "mem_service ops-certification-evidence-fixtures: bad checksum accepted\n");
        return 1;
    }
    printf("mem_service ops-certification-evidence-fixtures: status=ok "
           "evidence_schema=ops-certification-evidence-v1 "
           "positive=1 fail_closed=2 external_gates=6\n");
    return 0;
}

static int run_remote_transport_evidence_fixture_check(void)
{
    char valid[2048];
    char bad_gate[2048];
    char bad_topology[2048];
    char bad_checksum[2048];
    char reason[160];

    if (append_remote_transport_valid_evidence(valid, sizeof(valid)) != 0) {
        fprintf(stderr, "mem_service remote-transport-evidence-fixtures: render failed\n");
        return 1;
    }
    if (validate_remote_transport_evidence(valid, reason, sizeof(reason)) != 0) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: valid rejected "
                "reason=%s\n",
                reason);
        return 1;
    }
    strcpy(bad_gate, valid);
    {
        char *gate = strstr(bad_gate, "network_partition_fail_closed=pass\n");

        if (gate == NULL) {
            return 1;
        }
        memcpy(gate,
               "network_partition_fail_closed=fail\n",
               strlen("network_partition_fail_closed=fail\n"));
    }
    if (validate_remote_transport_evidence(bad_gate, reason, sizeof(reason)) == 0 ||
        strstr(reason, "network_partition_fail_closed") == NULL) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: bad gate accepted\n");
        return 1;
    }
    strcpy(bad_topology, valid);
    {
        char *topology = strstr(bad_topology, "transport_topology=cross-host\n");

        if (topology == NULL) {
            return 1;
        }
        memcpy(topology,
               "transport_topology=loopback   \n",
               strlen("transport_topology=loopback   \n"));
    }
    if (validate_remote_transport_evidence(bad_topology, reason, sizeof(reason)) == 0 ||
        strcmp(reason, "bad-evidence-identity") != 0) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: bad topology accepted\n");
        return 1;
    }
    strcpy(bad_checksum, valid);
    {
        char *checksum = strstr(bad_checksum, "package_manifest_checksum=0x");

        if (checksum == NULL) {
            return 1;
        }
        memcpy(checksum,
               "package_manifest_checksum=0x00000000",
               strlen("package_manifest_checksum=0x00000000"));
    }
    if (validate_remote_transport_evidence(bad_checksum, reason, sizeof(reason)) == 0 ||
        strcmp(reason, "bad-package-checksum") != 0) {
        fprintf(stderr,
                "mem_service remote-transport-evidence-fixtures: bad checksum accepted\n");
        return 1;
    }
    printf("mem_service remote-transport-evidence-fixtures: status=ok "
           "evidence_schema=remote-transport-evidence-v1 "
           "positive=1 fail_closed=3 external_gates=6 "
           "certification_status=not-certified-until-cross-host-evidence\n");
    return 0;
}

static int run_package_fixture_check(void)
{
    char manifest[8192];
    size_t used = 0;
    uint32_t checksum;

    if (render_package_manifest(manifest, sizeof(manifest), &used) != 0) {
        fprintf(stderr, "mem_service package-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(manifest, used);
    if (used != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service package-fixtures: manifest len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN);
        return 1;
    }
    if (checksum != MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service package-fixtures: manifest checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
        return 1;
    }
    if (strstr(manifest, "package_format=installed-layout-v1\n") == NULL ||
        strstr(manifest, "binary=bin/linqu_mem_service\n") == NULL ||
        strstr(manifest, "host_binary=libexec/lingqu/mem_service/linqu_mem_service_host\n") ==
            NULL ||
        strstr(manifest, "required_gate=install-smoke\n") == NULL ||
        strstr(manifest, "required_gate=installed-sdk-example-smoke\n") == NULL ||
        strstr(manifest, "artifact_gate=package-tarball-smoke\n") == NULL ||
        strstr(manifest, "native_package_gate=package-deb-smoke\n") == NULL ||
        strstr(manifest, "rpm_package_gate=package-rpm-smoke\n") == NULL ||
        strstr(manifest, "required_gate=upgrade-rollback-runtime-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=compat-runtime-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=alert-integration-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=ops-certification-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=ops-certification-evidence-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=remote-transport-evidence-fixtures\n") == NULL ||
        strstr(manifest,
               "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest, "required_gate=package-rpm-smoke\n") == NULL ||
        strstr(manifest, "required_gate=version-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=installed-sdk-runtime-smoke\n") == NULL ||
        strstr(manifest, "pkgconfig=lib/pkgconfig/lingqu-mem-service.pc\n") ==
            NULL ||
        strstr(manifest, "pkgconfig_name=lingqu-mem-service\n") == NULL ||
        strstr(manifest, "pkgconfig_cflags=-I${includedir}\n") == NULL ||
        strstr(manifest,
               "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c\n") ==
            NULL ||
        strstr(manifest, "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke\n") ==
            NULL ||
        strstr(manifest,
               "installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime\n") ==
            NULL ||
        strstr(manifest, "release_script_root=share/lingqu/mem_service/scripts\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest, "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh\n") ==
            NULL ||
        strstr(manifest,
               "release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh\n") ==
            NULL ||
        strstr(manifest, "file_class=release_scripts count=9\n") == NULL ||
        strstr(manifest, "file_class=pkgconfig count=1\n") == NULL ||
        strstr(manifest, "binary_version_command=version\n") == NULL ||
        strstr(manifest, "binary_version_contract=text-kv\n") == NULL ||
        strstr(manifest, "binary_version_gate=version-fixtures\n") == NULL ||
        strstr(manifest, "required_gate=restore-policy-fixtures\n") == NULL ||
        strstr(manifest, "contract=ops-certification-policy ") == NULL ||
        strstr(manifest, "payload_ownership_matrix=certified\n") == NULL ||
        strstr(manifest, "payload_ownership_scope=artifact-query-expected-owner\n") == NULL ||
        strstr(manifest, "service_auth_boundary=unix-socket-local-only\n") == NULL ||
        strstr(manifest, "metrics_auth_boundary=loopback-only\n") == NULL ||
        strstr(manifest, "config_security_gate=config-fixtures\n") == NULL ||
        strstr(manifest, "restore_policy=transactional-staged-restore\n") == NULL ||
        strstr(manifest, "restore_policy_gate=restore-policy-fixtures\n") == NULL ||
        strstr(manifest, "cross_version_upgrade=certified\n") == NULL) {
        fprintf(stderr, "mem_service package-fixtures: required manifest missing\n");
        return 1;
    }
    printf("mem_service package-fixtures: status=ok package_version=%u "
           "package_format=installed-layout-v1 manifest_len=%u "
           "manifest_checksum=0x%08x installed_files=%u required_gates=%u\n",
           MEM_SERVICE_PACKAGE_MANIFEST_VERSION,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT,
           MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT);
    return 0;
}

static int run_release_manifest(void)
{
    printf("mem_service_release_manifest_version=1\n");
    printf("service_name=linqu_mem_service\n");
    printf("service_version=%s\n", MEM_SERVICE_RELEASE_VERSION);
    printf("package_format=installed-layout-v1\n");
    printf("package_manifest=share/lingqu/mem_service/package-manifest.txt\n");
    printf("package_manifest_len=%u\n",
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN);
    printf("package_manifest_checksum=0x%08x\n",
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM);
    printf("package_gate=package-fixtures\n");
    printf("installed_sdk_example_smoke=installed-sdk-example-smoke\n");
    printf("installed_sdk_example_smoke_scope=serving+pretraining-external-client-compile\n");
    printf("installed_sdk_runtime_smoke=installed-sdk-runtime-smoke\n");
    printf("installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime\n");
    printf("distributable_package=out/mem_service/%s\n",
           MEM_SERVICE_PACKAGE_TARBALL_NAME);
    printf("distributable_package_format=tar\n");
    printf("distributable_package_root=usr+etc\n");
    printf("distributable_package_gate=package-tarball-smoke\n");
    printf("native_package=out/mem_service/%s\n", MEM_SERVICE_NATIVE_DEB_NAME);
    printf("native_package_format=deb\n");
    printf("native_package_arch=arm64\n");
    printf("native_package_gate=package-deb-smoke\n");
    printf("native_package_runtime=not-executed-cross-compiled-arm64\n");
    printf("rpm_native_package=out/mem_service/%s\n", MEM_SERVICE_NATIVE_RPM_NAME);
    printf("rpm_native_package_format=rpm\n");
    printf("rpm_native_package_arch=aarch64\n");
    printf("rpm_native_package_gate=package-rpm-smoke\n");
    printf("rpm_native_package_runtime=requires-linux-rpm-toolchain\n");
    printf("core_binary=bin/linqu_mem_service\n");
    printf("qwen3_adapter_binary_optional=bin/linqu_mem_service_qwen3\n");
    printf("host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host\n");
    printf("binary_version_command=version\n");
    printf("binary_version_contract=text-kv\n");
    printf("binary_version_gate=version-fixtures\n");
    printf("host_daemon_artifact_smoke=host-artifact-smoke\n");
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
    printf("api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt\n");
    printf("api_abi_policy_len=%u\n", MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN);
    printf("api_abi_policy_checksum=0x%08x\n",
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM);
    printf("admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt\n");
    printf("admin_output_schema_len=%u\n",
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN);
    printf("admin_output_schema_checksum=0x%08x\n",
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM);
    printf("admin_output_format=text-kv\n");
    printf("admin_metric_prefix=lingqu_mem_service_\n");
    printf("upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt\n");
    printf("upgrade_rollback_policy_len=%u\n",
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN);
    printf("upgrade_rollback_policy_checksum=0x%08x\n",
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM);
    printf("upgrade_policy=current-version-only\n");
    printf("rollback_policy=current-version-only\n");
    printf("old_server_runtime_binary=certified\n");
    printf("upgrade_rollback_gate=upgrade-rollback-fixtures\n");
    printf("upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures\n");
    printf("compat_runtime_gate=compat-runtime-fixtures\n");
    printf("compat_old_server_runtime_gate=compat-old-server-runtime-fixtures\n");
    printf("serving_fail_closed_matrix=certified\n");
    printf("serving_fail_closed_gate=serving-fail-closed-fixtures\n");
    printf("pretraining_fail_closed_matrix=certified\n");
    printf("pretraining_fail_closed_gate=pretraining-fail-closed-fixtures\n");
    printf("payload_ownership_matrix=certified\n");
    printf("payload_ownership_scope=artifact-query-expected-owner\n");
    printf("payload_ownership_gate=serving-fail-closed-fixtures,pretraining-fail-closed-fixtures\n");
    printf("restore_policy=transactional-staged-restore\n");
    printf("restore_policy_scope=full-snapshot+paged-snapshot\n");
    printf("restore_policy_gate=restore-policy-fixtures\n");
    printf("restore_policy_fail_closed=bad-magic,out-of-order-page,record-count-mismatch,cancelled-stage-commit\n");
    printf("restore_policy_live_state=unchanged-until-commit\n");
    printf("wire_payload_text_kv_format=text-kv\n");
    printf("wire_payload_typed_binary_format=typed-binary-v1\n");
    printf("wire_payload_typed_binary_gate=typed-payload-fixtures\n");
    printf("client_api_version=%u\n", MEM_SERVICE_CLIENT_API_VERSION);
    printf("client_abi_version=%u\n", MEM_SERVICE_CLIENT_ABI_VERSION);
    printf("client_record_abi_size=%u\n", MEM_SERVICE_CLIENT_RECORD_ABI_SIZE);
    printf("client_api_compatibility=%s\n",
           MEM_SERVICE_CLIENT_API_COMPATIBILITY);
    printf("client_abi_compatibility=%s\n",
           MEM_SERVICE_CLIENT_ABI_COMPATIBILITY);
    printf("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc\n");
    printf("pkgconfig_name=lingqu-mem-service\n");
    printf("pkgconfig_cflags=-I${includedir}\n");
    printf("pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c\n");
    printf("compat_matrix=share/lingqu/mem_service/compat-matrix.txt\n");
    printf("compat_matrix_len=%u\n", MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN);
    printf("compat_matrix_checksum=0x%08x\n",
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM);
    printf("compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt\n");
    printf("compat_baseline_len=%u\n",
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN);
    printf("compat_baseline_checksum=0x%08x\n",
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM);
    printf("compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt\n");
    printf("compat_old_new_matrix_len=%u\n",
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN);
    printf("compat_old_new_matrix_checksum=0x%08x\n",
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM);
    printf("config_schema_version=%u\n", MEM_SERVICE_CONFIG_SCHEMA_VERSION);
    printf("config_schema=share/lingqu/mem_service/config/mem_service.conf.schema\n");
    printf("config_example=share/lingqu/mem_service/config/mem_service.example.conf\n");
    printf("runtime_config=etc/lingqu/mem_service/mem_service.conf\n");
    printf("runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf\n");
    printf("host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf\n");
    printf("host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf\n");
    printf("service_auth_boundary=unix-socket-local-only\n");
    printf("metrics_auth_boundary=loopback-only\n");
    printf("config_security_gate=config-fixtures\n");
    printf("deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service\n");
    printf("host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service\n");
    printf("systemd_unit=lib/systemd/system/linqu_mem_service.service\n");
    printf("host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service\n");
    printf("deployment_smoke=deployment-fixtures\n");
    printf("host_service_manager_smoke=installed-host-service-manager-smoke\n");
    printf("host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm\n");
    printf("collector_smoke=collector-fixtures\n");
    printf("collector_integration_smoke=installed-host-collector-smoke\n");
    printf("collector_scrape_contract=prometheus-text-http-v0.0.4\n");
    printf("alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml\n");
    printf("alert_rules_format=prometheus-rules-yaml\n");
    printf("alert_rules_len=%u\n", MEM_SERVICE_ALERT_RULES_EXPECTED_LEN);
    printf("alert_rules_checksum=0x%08x\n",
           MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM);
    printf("alert_rule_count=%u\n", MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT);
    printf("alert_rules_gate=alert-fixtures\n");
    printf("alert_integration_smoke=alert-integration-fixtures\n");
    printf("ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt\n");
    printf("ops_certification_policy_len=%u\n",
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN);
    printf("ops_certification_policy_checksum=0x%08x\n",
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM);
    printf("ops_certification_gate=ops-certification-fixtures\n");
    printf("ops_certification_evidence_schema=ops-certification-evidence-v1\n");
    printf("ops_certification_evidence_gate=ops-certification-evidence-fixtures\n");
    printf("ops_certification_generate=ops-certification-generate-evidence\n");
    printf("ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke\n");
    printf("linux_ops_certification_smoke=linux-ops-certification-smoke\n");
    printf("linux_ops_evidence_verify=linux-ops-evidence-verify\n");
    printf("linux_ops_certification_bundle=linux-ops-certification-bundle\n");
    printf("linux_ops_certification_bundle_verify=linux-ops-certification-bundle-verify\n");
    printf("linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh\n");
    printf("linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight\n");
    printf("release_certification_verify=release-certification-verify\n");
    printf("release_certification_verify_script=scripts/verify_mem_service_release_certification.sh\n");
    printf("release_certification_ci=scripts/run_mem_service_release_certification_ci.sh\n");
    printf("release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight\n");
    printf("release_script_root=share/lingqu/mem_service/scripts\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/run_mem_service_linux_ops_ci.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_linux_ops_evidence.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_ops_certification_bundle.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/run_mem_service_remote_transport_ci.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_evidence.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_remote_transport_bundle.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/verify_mem_service_release_certification.sh\n");
    printf("release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh\n");
    printf("linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke\n");
    printf("linux_ops_deployment_smoke=linux-ops-deployment-smoke\n");
    printf("ops_certification_verify=ops-certification-verify --evidence-file\n");
    printf("real_systemd_environment=not-certified\n");
    printf("production_collector_alert_environment=not-certified\n");
    printf("rpm_package=not-certified\n");
    printf("service_manager_lifecycle=serve-config-ready-scrape-sigterm\n");
    printf("service_manager_shutdown=signal-clean-stop\n");
    printf("durable_backend=snapshot+journal\n");
    printf("durable_catalog=storage-root-v1\n");
    printf("durable_catalog_manifest=catalog/manifest.txt\n");
    printf("payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1\n");
    printf("remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1\n");
    printf("remote_payload_block_backend_gate=remote-block-backend-policy-fixtures\n");
    printf("remote_payload_block_data_gate=transport-block-fixtures\n");
    printf("remote_payload_network_transport=tcp-loopback-certified\n");
    printf("remote_payload_network_transport_gate=network-transport-block-fixtures\n");
    printf("remote_payload_network_transport_make_gate=network-transport-block-smoke\n");
    printf("remote_payload_production_network_transport=not-certified\n");
    printf("remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1\n");
    printf("remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures\n");
    printf("remote_payload_production_transport_generate=remote-transport-generate-evidence\n");
    printf("remote_payload_production_transport_verify=remote-transport-verify --evidence-file\n");
    printf("remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh\n");
    printf("remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight\n");
    printf("remote_payload_production_transport_evidence_verify=scripts/verify_mem_service_remote_transport_evidence.sh\n");
    printf("remote_payload_production_transport_bundle=remote-transport-certification-bundle\n");
    printf("remote_payload_production_transport_bundle_verify=remote-transport-certification-bundle-verify\n");
    printf("remote_payload_production_transport_bundle_script=scripts/verify_mem_service_remote_transport_bundle.sh\n");
    printf("payload_block_ingest=payload-inline,payload-file\n");
    printf("durable_snapshot=store-path\n");
    printf("durable_journal=store-path.journal\n");
    printf("metrics_export_format=prometheus-text\n");
    printf("metrics_listen_config=metrics_listen\n");
    printf("metrics_http_listener=tcp-ipv4\n");
    printf("metrics_scrape_path=/metrics\n");
    printf("metrics_http_content_type=text/plain; version=0.0.4\n");
    printf("client_retry_policy=explicit-max-attempts-backoff\n");
    printf("client_api=pretraining-refs-v1\n");
    printf("client_api=pretraining-step-commit-v1\n");
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
    printf("operation=audit_log:%u\n", MEM_SERVICE_WIRE_OP_AUDIT_LOG);
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
    if (MEM_SERVICE_CLIENT_API_VERSION != 1U ||
        MEM_SERVICE_CLIENT_ABI_VERSION != 1U ||
        MEM_SERVICE_CLIENT_RECORD_ABI_SIZE !=
            sizeof(struct mem_service_client_record)) {
        fprintf(stderr, "mem_service release-fixtures: api/abi policy mismatch\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN == 0U ||
        MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: schema manifest fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: api/abi policy fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN == 0U ||
        MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: admin output schema fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: upgrade/rollback policy fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_ALERT_RULES_EXPECTED_LEN == 0U ||
        MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM == 0U ||
        MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT != 5U) {
        fprintf(stderr, "mem_service release-fixtures: alert rules fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN == 0U ||
        MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: ops certification policy missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN == 0U ||
        MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM == 0U ||
        MEM_SERVICE_PACKAGE_MANIFEST_INSTALLED_FILE_COUNT != 45U ||
        MEM_SERVICE_PACKAGE_MANIFEST_GATE_COUNT != 26U) {
        fprintf(stderr, "mem_service release-fixtures: package manifest fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr, "mem_service release-fixtures: compat matrix fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: compat baseline fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN == 0U ||
        MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM == 0U) {
        fprintf(stderr,
                "mem_service release-fixtures: compat old/new fixture missing\n");
        failures -= 1;
    }
    if (MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT != 8U ||
        MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE != 9U ||
        MEM_SERVICE_WIRE_OP_AUDIT_LOG != 10U ||
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
    if (MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION != 1U) {
        fprintf(stderr, "mem_service release-fixtures: deployment smoke mismatch\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service release-fixtures: status=ok manifest_version=1 "
           "public_headers=8 client_sources=2 examples=2 config_artifacts=6 "
           "host_artifacts=1 "
           "package_artifacts=4 "
           "pkgconfig_artifacts=1 "
           "release_scripts=9 "
           "installed_sdk_runtime_smokes=1 "
           "version_smokes=1 "
           "config_security_smokes=1 "
           "systemd_units=2 "
           "deployment_smokes=1 service_manager_lifecycle_smokes=1 "
           "host_service_manager_smokes=1 "
           "collector_smokes=1 "
           "alert_rule_artifacts=1 alert_rules=%u "
           "alert_integration_smokes=1 "
           "ops_certification_policies=1 "
           "remote_transport_evidence_schemas=1 "
           "api_abi_policies=1 "
           "admin_output_schemas=1 "
           "upgrade_rollback_policies=1 "
           "upgrade_rollback_runtime_smokes=1 "
           "restore_policy_smokes=1 "
           "compat_runtime_smokes=1 "
           "durable_backends=1 durable_catalogs=1 payload_block_backends=4 "
           "metrics_export_formats=1 metrics_http_listeners=1 "
           "metrics_scrape_paths=1 "
           "client_retry_policies=1 "
           "client_api_profiles=2 compat_artifacts=3 "
           "operations=23 statuses=11 "
           "schema_manifest_len=%u schema_manifest_checksum=0x%08x "
           "api_abi_policy_len=%u api_abi_policy_checksum=0x%08x "
           "admin_output_schema_len=%u "
           "admin_output_schema_checksum=0x%08x "
           "upgrade_rollback_policy_len=%u "
           "upgrade_rollback_policy_checksum=0x%08x "
           "alert_rules_len=%u alert_rules_checksum=0x%08x "
           "ops_certification_policy_len=%u "
           "ops_certification_policy_checksum=0x%08x "
           "package_manifest_len=%u package_manifest_checksum=0x%08x "
           "compat_matrix_len=%u compat_matrix_checksum=0x%08x "
           "compat_baseline_len=%u compat_baseline_checksum=0x%08x "
           "compat_old_new_matrix_len=%u "
           "compat_old_new_matrix_checksum=0x%08x\n",
           MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN,
           MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN,
           MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_ALERT_RULES_EXPECTED_LEN,
           MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN,
           MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN,
           MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM,
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM,
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN,
           MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM);
    return 0;
}

static int run_remote_block_backend_policy_fixture_check(void)
{
    printf("mem_service remote-block-backend-policy-fixtures: status=ok "
           "remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1 "
           "remote_backend_admission=loopback-and-tcp-loopback-certified "
           "remote_payload_block_data_gate=transport-block-fixtures "
           "remote_payload_network_transport=tcp-loopback-certified "
           "remote_payload_network_transport_gate=network-transport-block-fixtures "
           "current_payload_block_backends=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1\n");
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
    bool has_storage_root;
    bool has_metrics_listen;
    char listen[160];
    char store[512];
    char storage_root[512];
    char metrics_listen[160];
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

static bool is_loopback_metrics_listen_spec(const char *value)
{
    const char *port_text;
    char *end = NULL;
    unsigned long port;

    if (value == NULL || strncmp(value, "tcp:127.0.0.1:", 14) != 0) {
        return false;
    }
    port_text = value + 14;
    if (port_text[0] == '\0') {
        return false;
    }
    errno = 0;
    port = strtoul(port_text, &end, 10);
    return errno == 0 && end != port_text && *end == '\0' &&
           port > 0UL && port <= 65535UL;
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
    if (strcmp(name, "storage_root") == 0) {
        if (copy_config_value(config->storage_root,
                              sizeof(config->storage_root),
                              value) != 0) {
            return -1;
        }
        config->has_storage_root = true;
        return 0;
    }
    if (strcmp(name, "metrics_listen") == 0) {
        if (!is_loopback_metrics_listen_spec(value) ||
            copy_config_value(config->metrics_listen,
                              sizeof(config->metrics_listen),
                              value) != 0) {
            return -1;
        }
        config->has_metrics_listen = true;
        return 0;
    }
    if (strcmp(name, "backend") == 0) {
        return strcmp(value, "snapshot") == 0 ||
                       strcmp(value, "snapshot+journal") == 0
                   ? 0
                   : -1;
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
                "backend=snapshot+journal\n"
                "max_records=1024\n"
                "max_payload_bytes=4096\n"
                "retention=manual\n"
                "auth_mode=none\n"
                "metrics_mode=text-kv\n"
                "metrics_listen=tcp:127.0.0.1:9900\n"
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
    if (fprintf(file,
                "listen=unix:/tmp/linqu_mem_service_fixture_bad.sock\n"
                "metrics_listen=tcp:0.0.0.0:9900\n") < 0) {
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
        !config.has_storage_root ||
        !config.has_metrics_listen ||
        strcmp(config.listen, "unix:/tmp/linqu_mem_service_fixture.sock") != 0 ||
        strcmp(config.store, "/tmp/linqu_mem_service_fixture.store") != 0 ||
        strcmp(config.storage_root, "/tmp/linqu_mem_service_fixture") != 0 ||
        strcmp(config.metrics_listen, "tcp:127.0.0.1:9900") != 0) {
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
    printf("mem_service config-fixtures: status=ok schema_version=%u listen=%s store=%s "
           "storage_root=%s service_auth_boundary=unix-socket-local-only "
           "metrics_auth_boundary=loopback-only\n",
           MEM_SERVICE_CONFIG_SCHEMA_VERSION,
           "unix:/tmp/linqu_mem_service_fixture.sock",
           "/tmp/linqu_mem_service_fixture.store",
           "/tmp/linqu_mem_service_fixture");
    return 0;
}

static int derive_store_from_storage_root(char *out,
                                          size_t out_len,
                                          const char *storage_root)
{
    size_t root_len;
    const char *separator = "/";

    if (out == NULL || out_len == 0 || storage_root == NULL ||
        storage_root[0] == '\0') {
        return -1;
    }
    root_len = strlen(storage_root);
    if (root_len > 0 && storage_root[root_len - 1U] == '/') {
        separator = "";
    }
    return snprintf(out,
                    out_len,
                    "%s%scatalog/store.snapshot",
                    storage_root,
                    separator) < (int)out_len
               ? 0
               : -1;
}

static int run_serve(int argc, char **argv)
{
    const char *config_path = option_value(argc, argv, "--config");
    const char *listen_override = option_value(argc, argv, "--listen");
    const char *store_override = option_value(argc, argv, "--store");
    const char *metrics_listen_override = option_value(argc, argv, "--metrics-listen");
    const char *listen_spec = mem_service_default_unix_socket_spec();
    const char *store_path = NULL;
    const char *metrics_listen_spec = NULL;
    const char *storage_root = NULL;
    char derived_store[512];
    struct mem_service_cli_config config;

    derived_store[0] = '\0';
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
        storage_root = config.has_storage_root ? config.storage_root : NULL;
        metrics_listen_spec =
            config.has_metrics_listen ? config.metrics_listen : metrics_listen_spec;
        if (store_path == NULL && storage_root != NULL &&
            derive_store_from_storage_root(derived_store,
                                           sizeof(derived_store),
                                           storage_root) != 0) {
            fprintf(stderr,
                    "mem_service: failed to derive store from storage_root=%s\n",
                    storage_root);
            return 2;
        }
        if (store_path == NULL && derived_store[0] != '\0') {
            store_path = derived_store;
        }
    }
    if (listen_override != NULL) {
        listen_spec = listen_override;
    }
    if (store_override != NULL) {
        store_path = store_override;
    }
    if (metrics_listen_override != NULL) {
        metrics_listen_spec = metrics_listen_override;
    }
    return mem_service_run_unix_daemon_with_store_metrics_and_catalog(listen_spec,
                                                                     store_path,
                                                                     metrics_listen_spec,
                                                                     storage_root);
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

static int render_admin_output_schema(char *schema, size_t schema_len, size_t *used_out)
{
    size_t used = 0;

    if (schema == NULL || schema_len == 0) {
        return -1;
    }
    schema[0] = '\0';
    if (append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "mem_service_admin_output_schema_version=%u\n",
                                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_VERSION) != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "service_name=linqu_mem_service\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_output_format=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "cli_status_line=mem_service <command>: status=<wire_status_name>\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "cli_payload_separator=payload-newline-for-payload-commands\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=health operation=health response=payload_optional\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=ready operation=ready response=payload_optional\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=status operation=status response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=list-records operation=list_records response=record-lines\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=metrics operation=metrics response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=metrics-export operation=metrics response=prometheus-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=audit-log operation=audit_log response=text-kv-records\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=inspect-object operation=inspect_object response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=export-snapshot operation=export_snapshot response=snapshot-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=export-snapshot-page operation=export_snapshot_page response=snapshot-page-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=export-snapshot-to operation=export_snapshot_page response=local-file-summary\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=restore-snapshot operation=restore_snapshot response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "admin_command=restore-snapshot-page operation=restore_snapshot_page response=text-kv\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=shmem_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=urma_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=block_ready type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=prefix_group_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=prefix_entry_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=kv_segment_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=object_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=runtime_handoff_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=execution_artifact_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "status_field=training_artifact_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "list_records_empty_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "list_records_record_line=record index=<u64> kind=<u32> kind_name=<string> key=<string> version=<u64> checksum=<u64>\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_export_format=prometheus-text\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_prometheus_prefix=lingqu_mem_service_\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_prometheus_default_type=counter\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metrics_prometheus_type=request_latency_max_ms:gauge\n") != 0) {
        return -1;
    }
#define APPEND_COUNTER_METRIC(name) \
    do { \
        if (append_wire_schema_line(schema, schema_len, &used, \
                                    "metric_field=" name " type=counter\n") != 0) { \
            return -1; \
        } \
    } while (0)
    APPEND_COUNTER_METRIC("request_count");
    APPEND_COUNTER_METRIC("ok_count");
    APPEND_COUNTER_METRIC("error_count");
    APPEND_COUNTER_METRIC("not_found_count");
    APPEND_COUNTER_METRIC("stale_ref_count");
    APPEND_COUNTER_METRIC("checksum_mismatch_count");
    APPEND_COUNTER_METRIC("version_conflict_count");
    APPEND_COUNTER_METRIC("invalid_model_binding_count");
    APPEND_COUNTER_METRIC("invalid_session_count");
    APPEND_COUNTER_METRIC("timeout_count");
    APPEND_COUNTER_METRIC("capacity_exceeded_count");
    APPEND_COUNTER_METRIC("unsupported_count");
    APPEND_COUNTER_METRIC("internal_count");
    APPEND_COUNTER_METRIC("fail_closed_count");
    APPEND_COUNTER_METRIC("health_count");
    APPEND_COUNTER_METRIC("ready_count");
    APPEND_COUNTER_METRIC("status_count");
    APPEND_COUNTER_METRIC("list_records_count");
    APPEND_COUNTER_METRIC("metrics_count");
    APPEND_COUNTER_METRIC("audit_log_count");
    APPEND_COUNTER_METRIC("export_snapshot_count");
    APPEND_COUNTER_METRIC("export_snapshot_page_count");
    APPEND_COUNTER_METRIC("restore_snapshot_count");
    APPEND_COUNTER_METRIC("restore_snapshot_page_count");
    APPEND_COUNTER_METRIC("put_object_count");
    APPEND_COUNTER_METRIC("get_object_count");
    APPEND_COUNTER_METRIC("inspect_object_count");
    APPEND_COUNTER_METRIC("get_object_hit_count");
    APPEND_COUNTER_METRIC("get_object_miss_count");
    APPEND_COUNTER_METRIC("register_prefix_count");
    APPEND_COUNTER_METRIC("lookup_prefix_count");
    APPEND_COUNTER_METRIC("prefix_lookup_hit_count");
    APPEND_COUNTER_METRIC("prefix_lookup_miss_count");
    APPEND_COUNTER_METRIC("publish_kv_count");
    APPEND_COUNTER_METRIC("resolve_kv_count");
    APPEND_COUNTER_METRIC("kv_resolve_hit_count");
    APPEND_COUNTER_METRIC("kv_resolve_miss_count");
    APPEND_COUNTER_METRIC("publish_runtime_handoff_count");
    APPEND_COUNTER_METRIC("resolve_runtime_handoff_count");
    APPEND_COUNTER_METRIC("register_execution_artifact_count");
    APPEND_COUNTER_METRIC("query_execution_artifact_count");
    APPEND_COUNTER_METRIC("register_training_artifact_count");
    APPEND_COUNTER_METRIC("query_training_artifact_count");
    APPEND_COUNTER_METRIC("artifact_query_hit_count");
    APPEND_COUNTER_METRIC("artifact_query_miss_count");
    APPEND_COUNTER_METRIC("idempotency_replay_count");
    APPEND_COUNTER_METRIC("idempotency_conflict_count");
    APPEND_COUNTER_METRIC("request_latency_total_ms");
    if (append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "metric_field=request_latency_max_ms type=gauge\n") != 0) {
        return -1;
    }
    APPEND_COUNTER_METRIC("request_latency_le_1ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_5ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_10ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_50ms_count");
    APPEND_COUNTER_METRIC("request_latency_le_100ms_count");
    APPEND_COUNTER_METRIC("request_latency_gt_100ms_count");
#undef APPEND_COUNTER_METRIC
    if (append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=audit_log type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=retained_events type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=first_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=start_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_record_delimiter=audit_begin/audit_end\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=monotonic_ms type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=operation type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=operation_name type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=status type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=status_name type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=request_checksum type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=response_checksum type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=idempotency_replay type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=key type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=session_id type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=model_key type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=artifact_kind type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=artifact_id type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=idempotency_key type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=version type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_event_field=checksum type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=events_emitted type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=next_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "audit_field=complete type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_export_magic=%s\n",
                                MEM_SERVICE_CLI_STORE_MAGIC) != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=audit_next_sequence type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_field=audit_event_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_record_delimiter=record_begin/record_end\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=snapshot_page type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=store_magic type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=start_index type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=next_index type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=records_emitted type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "snapshot_page_field=complete type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_field=status type=string\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_field=restored type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_field=record_count type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=restore_stage type=string enum=begun,appended,cancelled\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=expected_records type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=page_index type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=records_imported type=u64\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "restore_page_field=complete type=u32\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=stale_ref\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=checksum_mismatch\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=version_conflict\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=invalid_model_binding\n") != 0 ||
        append_wire_schema_line(schema,
                                schema_len,
                                &used,
                                "fail_closed_status=invalid_session\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static int run_admin_output_schema(void)
{
    char schema[8192];
    size_t used = 0;

    if (render_admin_output_schema(schema, sizeof(schema), &used) != 0) {
        fprintf(stderr, "mem_service admin-output-schema: render failed\n");
        return 1;
    }
    (void)used;
    fputs(schema, stdout);
    return 0;
}

static int run_admin_output_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=3\n"
        "request_latency_total_ms=11\n"
        "request_latency_max_ms=7\n";
    char schema[8192];
    char exported[1024];
    size_t used = 0;
    uint32_t checksum;
    int failures = 0;

    if (render_admin_output_schema(schema, sizeof(schema), &used) != 0) {
        fprintf(stderr, "mem_service admin-output-fixtures: render failed\n");
        return 1;
    }
    checksum = mem_service_wire_checksum(schema, used);
    if (used != MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service admin-output-fixtures: schema len actual=%zu "
                "expected=%u\n",
                used,
                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service admin-output-fixtures: schema checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (strstr(schema, "admin_command=status operation=status response=text-kv\n") ==
            NULL ||
        strstr(schema, "admin_command=metrics-export operation=metrics response=prometheus-text\n") ==
            NULL ||
        strstr(schema, "metric_field=request_latency_max_ms type=gauge\n") ==
            NULL ||
        strstr(schema, "audit_record_delimiter=audit_begin/audit_end\n") ==
            NULL ||
        strstr(schema, "snapshot_page_field=next_index type=u64\n") == NULL ||
        strstr(schema, "fail_closed_status=checksum_mismatch\n") == NULL) {
        fprintf(stderr, "mem_service admin-output-fixtures: required schema missing\n");
        failures -= 1;
    }
    if (render_metrics_prometheus_text(sample_metrics,
                                       exported,
                                       sizeof(exported)) != 0 ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_count counter\n"
               "lingqu_mem_service_request_count 3\n") == NULL ||
        strstr(exported,
               "# TYPE lingqu_mem_service_request_latency_max_ms gauge\n"
               "lingqu_mem_service_request_latency_max_ms 7\n") == NULL) {
        fprintf(stderr,
                "mem_service admin-output-fixtures: metrics export contract mismatch\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service admin-output-fixtures: status=ok schema_version=%u "
           "schema_len=%u schema_checksum=0x%08x admin_commands=13 "
           "metric_fields=55 prometheus_prefix=lingqu_mem_service_\n",
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_VERSION,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN,
           MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM);
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

static int render_metrics_http_response(const char *method,
                                        const char *path,
                                        const char *metrics_payload,
                                        char *output,
                                        size_t output_len)
{
    char body[8192];
    size_t used = 0;
    size_t body_len;

    if (method == NULL || path == NULL || output == NULL || output_len == 0 ||
        strcmp(method, "GET") != 0 || strcmp(path, "/metrics") != 0) {
        return -1;
    }
    if (render_metrics_prometheus_text(metrics_payload, body, sizeof(body)) != 0) {
        return -1;
    }
    body_len = strlen(body);
    output[0] = '\0';
    if (append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "HTTP/1.1 200 OK\r\n") != 0 ||
        append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "Content-Type: text/plain; version=0.0.4\r\n") != 0 ||
        append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "Content-Length: %zu\r\n",
                                   body_len) != 0 ||
        append_metrics_export_line(output,
                                   output_len,
                                   &used,
                                   "Cache-Control: no-store\r\n"
                                   "\r\n"
                                   "%s",
                                   body) != 0) {
        return -1;
    }
    return 0;
}

static int run_deployment_fixture_check(void)
{
    static const char deployment_manifest[] =
        "[Unit]\n"
        "Description=Lingqu Memory Service\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=/usr/bin/linqu_mem_service serve --config "
        "/etc/lingqu/mem_service/mem_service.conf\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";
    static const char host_deployment_manifest[] =
        "[Unit]\n"
        "Description=Lingqu Memory Service Host Daemon\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
        "serve --config /etc/lingqu/mem_service/mem_service.conf\n"
        "Restart=on-failure\n"
        "RestartSec=2\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";
    static const char sample_metrics[] =
        "request_count=5\n"
        "ok_count=5\n"
        "request_latency_max_ms=2\n";
    char response[4096];

    if (strstr(deployment_manifest, "[Service]\n") == NULL ||
        strstr(deployment_manifest,
               "ExecStart=/usr/bin/linqu_mem_service serve --config "
               "/etc/lingqu/mem_service/mem_service.conf\n") == NULL ||
        strstr(deployment_manifest, "Restart=on-failure\n") == NULL ||
        strstr(deployment_manifest, "WantedBy=multi-user.target\n") == NULL) {
        fprintf(stderr, "mem_service deployment-fixtures: manifest mismatch\n");
        return 1;
    }
    if (strstr(host_deployment_manifest, "[Service]\n") == NULL ||
        strstr(host_deployment_manifest,
               "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
               "serve --config /etc/lingqu/mem_service/mem_service.conf\n") == NULL ||
        strstr(host_deployment_manifest, "Restart=on-failure\n") == NULL ||
        strstr(host_deployment_manifest, "WantedBy=multi-user.target\n") == NULL) {
        fprintf(stderr,
                "mem_service deployment-fixtures: host manifest mismatch\n");
        return 1;
    }
    if (render_metrics_http_response("GET",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) != 0) {
        fprintf(stderr, "mem_service deployment-fixtures: http render failed\n");
        return 1;
    }
    if (strstr(response, "HTTP/1.1 200 OK\r\n") == NULL ||
        strstr(response, "Content-Type: text/plain; version=0.0.4\r\n") == NULL ||
        strstr(response, "Content-Length: ") == NULL ||
        strstr(response, "Cache-Control: no-store\r\n") == NULL ||
        strstr(response, "lingqu_mem_service_request_count 5\n") == NULL ||
        strstr(response,
               "# TYPE lingqu_mem_service_request_latency_max_ms gauge\n") == NULL) {
        fprintf(stderr, "mem_service deployment-fixtures: http response mismatch\n");
        return 1;
    }
    if (render_metrics_http_response("POST",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) == 0 ||
        render_metrics_http_response("GET",
                                     "/bad",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) == 0 ||
        render_metrics_http_response("GET",
                                     "/metrics",
                                     "bad-key=1\n",
                                     response,
                                     sizeof(response)) == 0) {
        fprintf(stderr,
                "mem_service deployment-fixtures: invalid http scrape accepted\n");
        return 1;
    }
    printf("mem_service deployment-fixtures: status=ok deployment_smoke_version=%u "
           "service_manager=systemd-like metrics_scrape_path=/metrics "
           "metrics_http_content_type=prometheus-text "
           "host_service_manager=systemd-like\n",
           MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION);
    return 0;
}

static bool collector_http_response_has_header(const char *response,
                                               const char *header)
{
    return response != NULL && header != NULL && strstr(response, header) != NULL;
}

static const char *collector_http_response_body(const char *response)
{
    const char *body;

    if (response == NULL) {
        return NULL;
    }
    body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        return NULL;
    }
    return body + 4;
}

static bool collector_metric_type_present(const char *body,
                                          const char *metric_name,
                                          const char *metric_type)
{
    char expected[256];

    if (body == NULL || metric_name == NULL || metric_type == NULL) {
        return false;
    }
    if (snprintf(expected,
                 sizeof(expected),
                 "# TYPE %s %s\n",
                 metric_name,
                 metric_type) >= (int)sizeof(expected)) {
        return false;
    }
    return strstr(body, expected) != NULL;
}

static bool collector_metric_value_at_least(const char *body,
                                            const char *metric_name,
                                            uint64_t minimum)
{
    const char *cursor = body;
    size_t metric_name_len;

    if (body == NULL || metric_name == NULL) {
        return false;
    }
    metric_name_len = strlen(metric_name);
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *value_start;
        char *value_end = NULL;
        uint64_t value;

        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        if ((size_t)(line_end - cursor) > metric_name_len &&
            strncmp(cursor, metric_name, metric_name_len) == 0 &&
            cursor[metric_name_len] == ' ') {
            value_start = cursor + metric_name_len + 1;
            value = strtoull(value_start, &value_end, 10);
            if (value_end == value_start) {
                return false;
            }
            while (value_end < line_end && isspace((unsigned char)*value_end)) {
                ++value_end;
            }
            return value_end == line_end && value >= minimum;
        }
        if (*line_end == '\0') {
            break;
        }
        cursor = line_end + 1;
    }
    return false;
}

static int run_collector_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=7\n"
        "ok_count=6\n"
        "health_count=1\n"
        "put_object_count=2\n"
        "request_latency_max_ms=4\n";
    char response[4096];
    const char *body;

    if (render_metrics_http_response("GET",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) != 0) {
        fprintf(stderr, "mem_service collector-fixtures: http render failed\n");
        return 1;
    }
    body = collector_http_response_body(response);
    if (!collector_http_response_has_header(response, "HTTP/1.1 200 OK\r\n") ||
        !collector_http_response_has_header(
            response,
            "Content-Type: text/plain; version=0.0.4\r\n") ||
        body == NULL) {
        fprintf(stderr, "mem_service collector-fixtures: http envelope failed\n");
        return 1;
    }
    if (!collector_metric_type_present(body,
                                       "lingqu_mem_service_request_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_request_latency_max_ms",
                                       "gauge") ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_request_count",
                                         7) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_health_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_put_object_count",
                                         2) ||
        collector_metric_value_at_least(body,
                                        "lingqu_mem_service_missing_count",
                                        1)) {
        fprintf(stderr, "mem_service collector-fixtures: metric parse failed\n");
        return 1;
    }
    printf("mem_service collector-fixtures: status=ok "
           "collector=prometheus-text-http metrics=5\n");
    return 0;
}

static int render_alert_rules(char *rules, size_t rules_len, size_t *used_out)
{
    size_t used = 0;

    if (rules == NULL || rules_len == 0) {
        return -1;
    }
    rules[0] = '\0';
    if (append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "# linqu mem_service prometheus alert rules\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "# contract_version: %u\n",
                                MEM_SERVICE_ALERT_RULES_VERSION) != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "groups:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "- name: lingqu_mem_service.rules\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  rules:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceDown\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: up{job=\"linqu_mem_service\"} == 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: critical\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: linqu_mem_service scrape target is down\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: check service manager, socket path, and metrics listener\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceErrorRate\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_error_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 5m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: warning\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service returned non-ok RPC statuses\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: inspect audit-log and recent client errors\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceFailClosed\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_fail_closed_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: critical\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service fail-closed path is active\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: check stale_ref, checksum_mismatch, and binding counters\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceChecksumMismatch\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: increase(lingqu_mem_service_checksum_mismatch_count[5m]) > 0\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 1m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: critical\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service detected a corrupt payload or ref\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: quarantine corrupt block and verify producer checksums\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "  - alert: LingquMemServiceHighLatency\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    expr: lingqu_mem_service_request_latency_max_ms > 100\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    for: 5m\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    labels:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      severity: warning\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      service: linqu_mem_service\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "    annotations:\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      summary: mem_service max request latency exceeded 100 ms\n") != 0 ||
        append_wire_schema_line(rules,
                                rules_len,
                                &used,
                                "      runbook: inspect storage backend, socket backlog, and client retry load\n") != 0) {
        return -1;
    }
    if (used_out != NULL) {
        *used_out = used;
    }
    return 0;
}

static size_t alert_rule_count(const char *rules)
{
    const char *cursor = rules;
    size_t count = 0;

    if (rules == NULL) {
        return 0;
    }
    while ((cursor = strstr(cursor, "  - alert: ")) != NULL) {
        ++count;
        cursor += strlen("  - alert: ");
    }
    return count;
}

static int run_alert_rules(void)
{
    char rules[8192];
    size_t used = 0;

    if (render_alert_rules(rules, sizeof(rules), &used) != 0) {
        fprintf(stderr, "mem_service alert-rules: render failed\n");
        return 1;
    }
    (void)used;
    fputs(rules, stdout);
    return 0;
}

static int run_alert_fixture_check(void)
{
    char rules[8192];
    size_t used = 0;
    size_t rule_count;
    uint32_t checksum;
    int failures = 0;

    if (render_alert_rules(rules, sizeof(rules), &used) != 0) {
        fprintf(stderr, "mem_service alert-fixtures: render failed\n");
        return 1;
    }
    rule_count = alert_rule_count(rules);
    checksum = mem_service_wire_checksum(rules, used);
    if (rule_count != MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT) {
        fprintf(stderr,
                "mem_service alert-fixtures: rule count actual=%zu expected=%u\n",
                rule_count,
                MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT);
        failures -= 1;
    }
    if (used != MEM_SERVICE_ALERT_RULES_EXPECTED_LEN) {
        fprintf(stderr,
                "mem_service alert-fixtures: rules len actual=%zu expected=%u\n",
                used,
                MEM_SERVICE_ALERT_RULES_EXPECTED_LEN);
        failures -= 1;
    }
    if (checksum != MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM) {
        fprintf(stderr,
                "mem_service alert-fixtures: rules checksum actual=0x%08x "
                "expected=0x%08x\n",
                checksum,
                MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM);
        failures -= 1;
    }
    if (strstr(rules, "alert: LingquMemServiceDown\n") == NULL ||
        strstr(rules, "lingqu_mem_service_fail_closed_count") == NULL ||
        strstr(rules, "lingqu_mem_service_checksum_mismatch_count") == NULL ||
        strstr(rules, "lingqu_mem_service_request_latency_max_ms") == NULL ||
        strstr(rules, "severity: critical\n") == NULL) {
        fprintf(stderr, "mem_service alert-fixtures: required alert missing\n");
        failures -= 1;
    }
    if (failures != 0) {
        return 1;
    }
    printf("mem_service alert-fixtures: status=ok format=prometheus-rules-yaml "
           "rules=%u rules_len=%u rules_checksum=0x%08x\n",
           MEM_SERVICE_ALERT_RULES_EXPECTED_RULE_COUNT,
           MEM_SERVICE_ALERT_RULES_EXPECTED_LEN,
           MEM_SERVICE_ALERT_RULES_EXPECTED_CHECKSUM);
    return 0;
}

static int run_alert_integration_fixture_check(void)
{
    static const char sample_metrics[] =
        "request_count=11\n"
        "error_count=1\n"
        "fail_closed_count=1\n"
        "checksum_mismatch_count=1\n"
        "request_latency_max_ms=101\n";
    char rules[8192];
    char response[4096];
    const char *body;
    size_t used = 0;

    if (render_alert_rules(rules, sizeof(rules), &used) != 0) {
        fprintf(stderr, "mem_service alert-integration-fixtures: rules render failed\n");
        return 1;
    }
    if (render_metrics_http_response("GET",
                                     "/metrics",
                                     sample_metrics,
                                     response,
                                     sizeof(response)) != 0) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: metrics render failed\n");
        return 1;
    }
    body = collector_http_response_body(response);
    if (body == NULL ||
        !collector_http_response_has_header(response, "HTTP/1.1 200 OK\r\n") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_error_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_fail_closed_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_checksum_mismatch_count",
                                       "counter") ||
        !collector_metric_type_present(body,
                                       "lingqu_mem_service_request_latency_max_ms",
                                       "gauge")) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: metrics contract missing\n");
        return 1;
    }
    if (strstr(rules, "up{job=\"linqu_mem_service\"} == 0") == NULL ||
        strstr(rules, "increase(lingqu_mem_service_error_count[5m]) > 0") == NULL ||
        strstr(rules, "increase(lingqu_mem_service_fail_closed_count[5m]) > 0") ==
            NULL ||
        strstr(rules,
               "increase(lingqu_mem_service_checksum_mismatch_count[5m]) > 0") ==
            NULL ||
        strstr(rules, "lingqu_mem_service_request_latency_max_ms > 100") == NULL) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: alert expression missing\n");
        return 1;
    }
    if (!collector_metric_value_at_least(body,
                                         "lingqu_mem_service_error_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_fail_closed_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_checksum_mismatch_count",
                                         1) ||
        !collector_metric_value_at_least(body,
                                         "lingqu_mem_service_request_latency_max_ms",
                                         101)) {
        fprintf(stderr,
                "mem_service alert-integration-fixtures: metric values missing\n");
        return 1;
    }
    printf("mem_service alert-integration-fixtures: status=ok "
           "collector=prometheus-text-http alert_rules=5 referenced_metrics=4 "
           "synthetic_targets=1\n");
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
    char payload[1024] = "";

    if (append_required_payload_field(payload, sizeof(payload), argc, argv, "--key", "key") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--backing-len", "backing_len") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--checksum", "checksum") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--version", "version") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-inline", "payload_inline") != 0 ||
        append_optional_payload_field(payload, sizeof(payload), argc, argv, "--payload-file", "payload_path") != 0 ||
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

static int run_audit_log(int argc, char **argv)
{
    char payload[160] = "";

    if (append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--start-sequence",
                                      "start_sequence") != 0 ||
        append_optional_payload_field(payload,
                                      sizeof(payload),
                                      argc,
                                      argv,
                                      "--max-events",
                                      "max_events") != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_AUDIT_LOG,
                                      "audit-log",
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
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-inline", "payload_inline") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-file", "payload_path") != 0 ||
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
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-owner", "expected_owner") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-version", "expected_version") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-checksum", "expected_checksum") != 0) {
        return -1;
    }
    return 0;
}

static int run_publish_runtime_handoff(int argc, char **argv)
{
    char payload[1024] = "";

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
    char payload[1024] = "";

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
    char payload[1024] = "";

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

static int append_training_step_commit_payload(char *payload,
                                               size_t payload_len,
                                               int argc,
                                               char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--session-id", "session_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--request-id", "request_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--model-key", "model_key") != 0 ||
        append_payload_field(payload,
                             payload_len,
                             "artifact_kind",
                             MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND) != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--artifact-id", "artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--owner", "owner") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-kind", "payload_kind") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-offset", "backing_offset") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--backing-len", "backing_len") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--checksum", "checksum") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--version", "version") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-inline", "payload_inline") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--payload-file", "payload_path") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--idempotency-key", "idempotency_key") != 0) {
        return -1;
    }
    return 0;
}

static int append_training_step_query_payload(char *payload,
                                              size_t payload_len,
                                              int argc,
                                              char **argv)
{
    if (append_required_payload_field(payload, payload_len, argc, argv, "--key", "key") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-session-id", "expected_session_id") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-model-key", "expected_model_key") != 0 ||
        append_payload_field(payload,
                             payload_len,
                             "expected_artifact_kind",
                             MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND) != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-artifact-id", "expected_artifact_id") != 0 ||
        append_optional_payload_field(payload, payload_len, argc, argv, "--expected-owner", "expected_owner") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-version", "expected_version") != 0 ||
        append_required_payload_field(payload, payload_len, argc, argv, "--expected-checksum", "expected_checksum") != 0) {
        return -1;
    }
    return 0;
}

static int run_commit_training_step(int argc, char **argv)
{
    char payload[1024] = "";

    if (append_training_step_commit_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT,
                                      "commit-training-step",
                                      payload);
}

static int run_resolve_training_step(int argc, char **argv)
{
    char payload[512] = "";

    if (append_training_step_query_payload(payload, sizeof(payload), argc, argv) != 0) {
        return 2;
    }
    return run_client_payload_command(argc,
                                      argv,
                                      MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT,
                                      "resolve-training-step",
                                      payload);
}

int main(int argc, char **argv)
{
    if (argc == 1 ||
        strcmp(argv[1], "--smoke") == 0 ||
        strcmp(argv[1], "--self-test") == 0) {
        return run_smoke();
    }
    if (strcmp(argv[1], "version") == 0) {
        return run_version_manifest();
    }
    if (strcmp(argv[1], "version-fixtures") == 0) {
        return run_version_fixture_check();
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
    if (strcmp(argv[1], "journal-fixtures") == 0) {
        return mem_service_run_journal_fixture_check();
    }
    if (strcmp(argv[1], "journal-compaction-fixtures") == 0) {
        return mem_service_run_journal_compaction_fixture_check();
    }
    if (strcmp(argv[1], "journal-torn-recovery-fixtures") == 0) {
        return mem_service_run_journal_torn_recovery_fixture_check();
    }
    if (strcmp(argv[1], "config-fixtures") == 0) {
        return run_config_fixture_check();
    }
    if (strcmp(argv[1], "metrics-export-fixtures") == 0) {
        return run_metrics_export_fixture_check();
    }
    if (strcmp(argv[1], "collector-fixtures") == 0) {
        return run_collector_fixture_check();
    }
    if (strcmp(argv[1], "alert-rules") == 0) {
        return run_alert_rules();
    }
    if (strcmp(argv[1], "alert-fixtures") == 0) {
        return run_alert_fixture_check();
    }
    if (strcmp(argv[1], "alert-integration-fixtures") == 0) {
        return run_alert_integration_fixture_check();
    }
    if (strcmp(argv[1], "ops-certification-policy") == 0) {
        return run_ops_certification_policy();
    }
    if (strcmp(argv[1], "ops-certification-fixtures") == 0) {
        return run_ops_certification_fixture_check();
    }
    if (strcmp(argv[1], "ops-certification-evidence-fixtures") == 0) {
        return run_ops_certification_evidence_fixture_check();
    }
    if (strcmp(argv[1], "ops-certification-generate-evidence") == 0) {
        return run_ops_certification_generate_evidence(argc, argv);
    }
    if (strcmp(argv[1], "ops-certification-linux-ci-smoke") == 0) {
        return run_ops_certification_linux_ci_smoke(argc, argv);
    }
    if (strcmp(argv[1], "ops-certification-verify") == 0) {
        return run_ops_certification_verify(argc, argv);
    }
    if (strcmp(argv[1], "deployment-fixtures") == 0) {
        return run_deployment_fixture_check();
    }
    if (strcmp(argv[1], "admin-output-schema") == 0) {
        return run_admin_output_schema();
    }
    if (strcmp(argv[1], "admin-output-fixtures") == 0) {
        return run_admin_output_fixture_check();
    }
    if (strcmp(argv[1], "upgrade-rollback-policy") == 0) {
        return run_upgrade_rollback_policy();
    }
    if (strcmp(argv[1], "upgrade-rollback-fixtures") == 0) {
        return run_upgrade_rollback_fixture_check();
    }
    if (strcmp(argv[1], "upgrade-rollback-runtime-fixtures") == 0) {
        return mem_service_run_upgrade_rollback_runtime_fixture_check();
    }
    if (strcmp(argv[1], "restore-policy-fixtures") == 0) {
        return mem_service_run_restore_policy_fixture_check();
    }
    if (strcmp(argv[1], "durable-catalog-fixtures") == 0) {
        return mem_service_run_durable_catalog_fixture_check();
    }
    if (strcmp(argv[1], "chunked-block-fixtures") == 0) {
        return mem_service_run_chunked_block_fixture_check();
    }
    if (strcmp(argv[1], "transport-block-fixtures") == 0) {
        return mem_service_run_transport_block_fixture_check();
    }
    if (strcmp(argv[1], "network-transport-block-fixtures") == 0) {
        return mem_service_run_network_transport_block_fixture_check();
    }
    if (strcmp(argv[1], "remote-block-backend-policy-fixtures") == 0) {
        return run_remote_block_backend_policy_fixture_check();
    }
    if (strcmp(argv[1], "remote-transport-evidence-fixtures") == 0) {
        return run_remote_transport_evidence_fixture_check();
    }
    if (strcmp(argv[1], "remote-transport-generate-evidence") == 0) {
        return run_remote_transport_generate_evidence(argc, argv);
    }
    if (strcmp(argv[1], "remote-transport-verify") == 0) {
        return run_remote_transport_verify(argc, argv);
    }
    if (strcmp(argv[1], "client-retry-fixtures") == 0) {
        return run_client_retry_fixture_check();
    }
    if (strcmp(argv[1], "api-abi-policy") == 0) {
        return run_api_abi_policy();
    }
    if (strcmp(argv[1], "api-abi-fixtures") == 0) {
        return run_api_abi_fixture_check();
    }
    if (strcmp(argv[1], "compat-matrix") == 0) {
        return run_compat_matrix();
    }
    if (strcmp(argv[1], "compat-fixtures") == 0) {
        return run_compat_fixture_check();
    }
    if (strcmp(argv[1], "compat-baseline-v1") == 0) {
        return run_compat_baseline_v1();
    }
    if (strcmp(argv[1], "compat-baseline-fixtures") == 0) {
        return run_compat_baseline_fixture_check();
    }
    if (strcmp(argv[1], "compat-old-new-matrix") == 0) {
        return run_compat_old_new_matrix();
    }
    if (strcmp(argv[1], "compat-old-new-fixtures") == 0) {
        return run_compat_old_new_fixture_check();
    }
    if (strcmp(argv[1], "compat-runtime-fixtures") == 0) {
        return mem_service_run_compat_runtime_fixture_check();
    }
    if (strcmp(argv[1], "compat-old-server-runtime-fixtures") == 0) {
        return mem_service_run_compat_old_server_runtime_fixture_check();
    }
    if (strcmp(argv[1], "serving-fail-closed-fixtures") == 0) {
        return mem_service_run_serving_fail_closed_fixture_check();
    }
    if (strcmp(argv[1], "pretraining-fail-closed-fixtures") == 0) {
        return mem_service_run_pretraining_fail_closed_fixture_check();
    }
    if (strcmp(argv[1], "typed-payload-fixtures") == 0) {
        return mem_service_run_typed_payload_fixture_check();
    }
    if (strcmp(argv[1], "package-manifest") == 0) {
        return run_package_manifest();
    }
    if (strcmp(argv[1], "package-fixtures") == 0) {
        return run_package_fixture_check();
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
    if (strcmp(argv[1], "audit-log") == 0) {
        return run_audit_log(argc, argv);
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
    if (strcmp(argv[1], "commit-training-step") == 0) {
        return run_commit_training_step(argc, argv);
    }
    if (strcmp(argv[1], "resolve-training-step") == 0) {
        return run_resolve_training_step(argc, argv);
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
