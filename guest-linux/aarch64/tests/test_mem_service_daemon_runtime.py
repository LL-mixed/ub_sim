import os
import shlex
import shutil
import socket
import subprocess
import tarfile
import tempfile
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
CLI_SOURCE = ROOT / "apps" / "mem_service" / "mem_service.c"
WIRE_SCHEMA_MANIFEST = ROOT / "apps" / "mem_service" / "wire-schema.txt"
ADMIN_OUTPUT_SCHEMA = ROOT / "apps" / "mem_service" / "admin-output-schema.txt"
UPGRADE_ROLLBACK_POLICY = ROOT / "apps" / "mem_service" / "upgrade-rollback-policy.txt"
PACKAGE_MANIFEST = ROOT / "apps" / "mem_service" / "package-manifest.txt"
OPS_CERTIFICATION_POLICY = (
    ROOT / "apps" / "mem_service" / "ops-certification-policy.txt"
)
ALERT_RULES = (
    ROOT / "apps" / "mem_service" / "deploy" / "linqu_mem_service.prometheus-alerts.yml"
)
API_ABI_POLICY = ROOT / "apps" / "mem_service" / "api-abi-policy.txt"
COMPAT_MATRIX = ROOT / "apps" / "mem_service" / "compat-matrix.txt"
COMPAT_BASELINE_V1 = ROOT / "apps" / "mem_service" / "compat-baseline-v1.txt"
COMPAT_OLD_NEW_MATRIX = ROOT / "apps" / "mem_service" / "compat-old-new-matrix.txt"
SDK_EXAMPLES_DIR = ROOT / "apps" / "mem_service" / "examples"


def _tmp_parent() -> Path:
    private_tmp = Path("/private/tmp")
    if private_tmp.exists():
        return private_tmp
    return Path(tempfile.gettempdir())


def _fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceWireClientBuildTests(unittest.TestCase):
    def test_wire_and_typed_clients_build_without_daemon_or_core(self):
        with tempfile.TemporaryDirectory(prefix="msvc_client_", dir=str(_tmp_parent())) as tmp:
            tmp_path = Path(tmp)
            source = tmp_path / "wire_client_smoke.c"
            binary = tmp_path / "wire_client_smoke"

            source.write_text(
                '#include "components/mem_service/mem_service_wire_client.h"\n'
                '#include "components/mem_service/mem_service_client.h"\n'
                "int main(void) {\n"
                "    struct mem_service_client client;\n"
                "    struct mem_service_client_object object = {\n"
                "        .key = \"sealed-object\",\n"
                "        .has_payload_kind = true,\n"
                "        .payload_kind = MEM_SERVICE_CLIENT_PAYLOAD_KIND_SEALED_LOCAL_BLOCK,\n"
                "        .payload_inline = \"sealed-payload\",\n"
                "        .payload_path = \"/tmp/sealed-payload.bin\",\n"
                "    };\n"
                "    struct mem_service_wire_client_options options = {\n"
                "        .timeout_ms = 25,\n"
                "    };\n"
                "    const char *status = mem_service_wire_status_name("
                "MEM_SERVICE_WIRE_STATUS_OK);\n"
                "    const char *spec = mem_service_default_unix_socket_spec();\n"
                "    mem_service_client_init_with_options(&client, spec, &options);\n"
                "    return status != 0 && client.connect_spec == spec && "
                "client.wire_options.timeout_ms == 25 && object.payload_inline != 0 && "
                "object.payload_path != 0 && "
                "object.payload_kind == MEM_SERVICE_CLIENT_PAYLOAD_KIND_SEALED_LOCAL_BLOCK && "
                "MEM_SERVICE_CLIENT_API_VERSION == 1 && "
                "MEM_SERVICE_CLIENT_ABI_VERSION == 1 && "
                "sizeof(struct mem_service_client_record) == "
                "MEM_SERVICE_CLIENT_RECORD_ABI_SIZE "
                "? 0 : 1;\n"
                "}\n"
            )
            cmd = [
                "cc",
                "-O2",
                "-Wall",
                "-Wextra",
                f"-I{ROOT}",
                f"-I{ROOT.parent}",
                str(source),
                str(SERVICE_DIR / "mem_service_client.c"),
                str(SERVICE_DIR / "mem_service_wire_client.c"),
                "-o",
                str(binary),
            ]
            subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], cwd=REPO_ROOT, check=True)


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceDaemonRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="msvc_", dir=str(_tmp_parent())))
        self.binary = self.root / "linqu_mem_service"
        self.client_binary = self.root / "mem_service_typed_client"
        self.socket = _tmp_parent() / f"linqu_mem_service_{os.getpid()}_{id(self)}.sock"
        self.store = self.root / "service.store"
        self._compile_host_binary()
        self._compile_typed_client_binary()

    def tearDown(self):
        self.socket.unlink(missing_ok=True)
        shutil.rmtree(self.root, ignore_errors=True)

    def _compile_host_binary(self):
        cmd = [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ROOT}",
            f"-I{ROOT.parent}",
            f"-I{ROOT / 'libs' / 'obmm_queue'}",
            f"-I{ROOT / 'apps' / 'obmm_queue'}",
            str(CLI_SOURCE),
            str(SERVICE_DIR / "mem_service_daemon.c"),
            str(SERVICE_DIR / "mem_service_client.c"),
            str(SERVICE_DIR / "mem_service_wire_client.c"),
            str(SERVICE_DIR / "mem_service_metadata.c"),
            str(SERVICE_DIR / "mem_service_keys.c"),
            str(SERVICE_DIR / "mem_service_object_refs.c"),
            str(SERVICE_DIR / "mem_service_records.c"),
            "-lm",
            "-o",
            str(self.binary),
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _compile_typed_client_binary(self):
        source = self.root / "mem_service_typed_client.c"
        source.write_text(
            '#include <stdio.h>\n'
            '#include <string.h>\n'
            '#include "components/mem_service/mem_service_client.h"\n'
            "\n"
            "static int fail(const char *message) {\n"
            "    fprintf(stderr, \"%s\\n\", message);\n"
            "    return 1;\n"
            "}\n"
            "\n"
            "static int expect_ok(int rc, enum mem_service_wire_status status, const char *op) {\n"
            "    if (rc != 0 || status != MEM_SERVICE_WIRE_STATUS_OK) {\n"
            "        fprintf(stderr, \"%s failed rc=%d status=%u\\n\", op, rc, (unsigned)status);\n"
            "        return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
            "\n"
            "int main(int argc, char **argv) {\n"
            "    struct mem_service_client client;\n"
            "    struct mem_service_client_record record;\n"
            "    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;\n"
            "    char status_payload[512];\n"
            "    char snapshot_payload[4096];\n"
            "    char snapshot_page_payload[4096];\n"
            "    char restore_payload[256];\n"
            "    struct mem_service_client_object object = {\n"
            "        .key = \"typed-object\",\n"
            "        .has_backing_len = true,\n"
            "        .backing_len = 123,\n"
            "        .has_checksum = true,\n"
            "        .checksum = 99,\n"
            "        .has_version = true,\n"
            "        .version = 11,\n"
            "    };\n"
            "    struct mem_service_client_block_entry prefix = {\n"
            "        .request_id = \"typed-request\",\n"
            "        .prefix_group = \"typed-prefix\",\n"
            "        .group_id = \"typed-group\",\n"
            "        .block_hash = \"typed-block\",\n"
            "        .has_placement_node = true,\n"
            "        .placement_node = 1,\n"
            "        .has_placement_level = true,\n"
            "        .placement_level = 2,\n"
            "        .has_hot_segment_id = true,\n"
            "        .hot_segment_id = 4096,\n"
            "        .state = \"filled\",\n"
            "        .has_result_segment_id = true,\n"
            "        .result_segment_id = 8192,\n"
            "    };\n"
            "    struct mem_service_client_block_entry kv = {\n"
            "        .request_id = \"typed-request\",\n"
            "        .prefix_group = \"typed-prefix\",\n"
            "        .group_id = \"typed-group\",\n"
            "        .block_hash = \"typed-kv-block\",\n"
            "        .state = \"filled\",\n"
            "        .has_result_segment_id = true,\n"
            "        .result_segment_id = 12288,\n"
            "    };\n"
            "    struct mem_service_client_kv_selector kv_selector = {\n"
            "        .block_hash = \"typed-kv-block\",\n"
            "    };\n"
            "    struct mem_service_client_artifact runtime_artifact = {\n"
            "        .key = \"runtime/session/range-0\",\n"
            "        .session_id = \"session\",\n"
            "        .model_key = \"model-a\",\n"
            "        .artifact_kind = \"hidden-range\",\n"
            "        .artifact_id = \"range-0\",\n"
            "        .has_backing_len = true,\n"
            "        .backing_len = 512,\n"
            "        .has_checksum = true,\n"
            "        .checksum = 777,\n"
            "        .has_version = true,\n"
            "        .version = 3,\n"
            "    };\n"
            "    struct mem_service_client_artifact_query runtime_query = {\n"
            "        .key = \"runtime/session/range-0\",\n"
            "        .expected_session_id = \"session\",\n"
            "        .expected_model_key = \"model-a\",\n"
            "        .expected_artifact_kind = \"hidden-range\",\n"
            "        .expected_artifact_id = \"range-0\",\n"
            "        .has_expected_version = true,\n"
            "        .expected_version = 3,\n"
            "        .has_expected_checksum = true,\n"
            "        .expected_checksum = 777,\n"
            "    };\n"
            "    struct mem_service_client_artifact execution_artifact = {\n"
            "        .key = \"execution/session/logits-0\",\n"
            "        .session_id = \"session\",\n"
            "        .model_key = \"model-a\",\n"
            "        .artifact_kind = \"logits\",\n"
            "        .artifact_id = \"logits-0\",\n"
            "        .has_checksum = true,\n"
            "        .checksum = 888,\n"
            "        .has_version = true,\n"
            "        .version = 4,\n"
            "    };\n"
            "    struct mem_service_client_artifact_query execution_query = {\n"
            "        .key = \"execution/session/logits-0\",\n"
            "        .expected_session_id = \"session\",\n"
            "        .expected_model_key = \"model-a\",\n"
            "        .expected_artifact_kind = \"logits\",\n"
            "        .expected_artifact_id = \"logits-0\",\n"
            "        .has_expected_version = true,\n"
            "        .expected_version = 4,\n"
            "        .has_expected_checksum = true,\n"
            "        .expected_checksum = 888,\n"
            "    };\n"
            "    struct mem_service_client_training_ref training_ref = {\n"
            "        .key = \"training/run/checkpoint-0\",\n"
            "        .session_id = \"run\",\n"
            "        .model_key = \"model-a\",\n"
            "        .artifact_id = \"checkpoint-0\",\n"
            "        .has_backing_len = true,\n"
            "        .backing_len = 4096,\n"
            "        .has_checksum = true,\n"
            "        .checksum = 999,\n"
            "        .has_version = true,\n"
            "        .version = 5,\n"
            "    };\n"
            "    struct mem_service_client_training_ref_query training_query = {\n"
            "        .key = \"training/run/checkpoint-0\",\n"
            "        .expected_session_id = \"run\",\n"
            "        .expected_model_key = \"model-a\",\n"
            "        .expected_artifact_id = \"checkpoint-0\",\n"
            "        .has_expected_version = true,\n"
            "        .expected_version = 5,\n"
            "        .has_expected_checksum = true,\n"
            "        .expected_checksum = 999,\n"
            "    };\n"
            "    struct mem_service_client_training_ref step_commit = {\n"
            "        .key = \"training/run/global-step-0/commit\",\n"
            "        .idempotency_key = \"training/run/global-step-0/commit/v6\",\n"
            "        .session_id = \"run\",\n"
            "        .request_id = \"global-step-0\",\n"
            "        .model_key = \"model-a\",\n"
            "        .artifact_id = \"global-step-0\",\n"
            "        .has_checksum = true,\n"
            "        .checksum = 1000,\n"
            "        .has_version = true,\n"
            "        .version = 6,\n"
            "    };\n"
            "    struct mem_service_client_training_ref_query step_query = {\n"
            "        .key = \"training/run/global-step-0/commit\",\n"
            "        .expected_session_id = \"run\",\n"
            "        .expected_model_key = \"model-a\",\n"
            "        .expected_artifact_id = \"global-step-0\",\n"
            "        .has_expected_version = true,\n"
            "        .expected_version = 6,\n"
            "        .has_expected_checksum = true,\n"
            "        .expected_checksum = 1000,\n"
            "    };\n"
            "\n"
            "    if (argc != 2) {\n"
            "        return fail(\"missing socket spec\");\n"
            "    }\n"
            "    mem_service_client_init(&client, argv[1]);\n"
            "    if (expect_ok(mem_service_client_health(&client, &status), status, \"health\") != 0) return 1;\n"
            "    if (expect_ok(mem_service_client_ready(&client, &status), status, \"ready\") != 0) return 1;\n"
            "    memset(status_payload, 0, sizeof(status_payload));\n"
            "    if (expect_ok(mem_service_client_status(&client, status_payload, sizeof(status_payload), &status), status, \"status\") != 0) return 1;\n"
            "    if (strstr(status_payload, \"record_count=\") == NULL) return fail(\"missing status payload\");\n"
            "    if (expect_ok(mem_service_client_put_object(&client, &object, &record, &status), status, \"put_object\") != 0) return 1;\n"
            "    if (record.version != 11) return fail(\"put_object version mismatch\");\n"
            "    if (expect_ok(mem_service_client_get_object(&client, \"typed-object\", &record, &status), status, \"get_object\") != 0) return 1;\n"
            "    if (record.object_backing_len != 123 || record.object_payload_checksum != 99) return fail(\"get_object payload mismatch\");\n"
            "    if (expect_ok(mem_service_client_inspect_object(&client, \"typed-object\", &record, &status), status, \"inspect_object\") != 0) return 1;\n"
            "    if (record.kind != 5U || record.object_payload_checksum != 99) return fail(\"inspect_object payload mismatch\");\n"
            "    memset(snapshot_payload, 0, sizeof(snapshot_payload));\n"
            "    if (expect_ok(mem_service_client_export_snapshot(&client, snapshot_payload, sizeof(snapshot_payload), &status), status, \"export_snapshot\") != 0) return 1;\n"
            "    if (strstr(snapshot_payload, \"mem_service_store_v1\") == NULL) return fail(\"snapshot missing magic\");\n"
            "    if (strstr(snapshot_payload, \"key=typed-object\") == NULL) return fail(\"snapshot missing object\");\n"
            "    memset(snapshot_page_payload, 0, sizeof(snapshot_page_payload));\n"
            "    if (expect_ok(mem_service_client_export_snapshot_page(&client, 0, 1, snapshot_page_payload, sizeof(snapshot_page_payload), &status), status, \"export_snapshot_page\") != 0) return 1;\n"
            "    if (strstr(snapshot_page_payload, \"snapshot_page=1\") == NULL) return fail(\"snapshot page missing marker\");\n"
            "    if (strstr(snapshot_page_payload, \"key=typed-object\") == NULL) return fail(\"snapshot page missing object\");\n"
            "    if (strstr(snapshot_page_payload, \"complete=1\") == NULL) return fail(\"snapshot page should complete\");\n"
            "    memset(restore_payload, 0, sizeof(restore_payload));\n"
            "    if (expect_ok(mem_service_client_restore_snapshot(&client, snapshot_payload, restore_payload, sizeof(restore_payload), &status), status, \"restore_snapshot\") != 0) return 1;\n"
            "    if (strstr(restore_payload, \"restored=1\") == NULL) return fail(\"restore missing marker\");\n"
            "    if (expect_ok(mem_service_client_register_prefix_entry(&client, &prefix, &record, &status), status, \"register_prefix\") != 0) return 1;\n"
            "    if (strcmp(record.block_hash, \"typed-block\") != 0) return fail(\"prefix block mismatch\");\n"
            "    if (expect_ok(mem_service_client_lookup_prefix_entry(&client, \"typed-request\", \"typed-prefix\", &record, &status), status, \"lookup_prefix\") != 0) return 1;\n"
            "    if (strcmp(record.state, \"filled\") != 0) return fail(\"prefix state mismatch\");\n"
            "    if (expect_ok(mem_service_client_publish_kv_segment(&client, &kv, &record, &status), status, \"publish_kv\") != 0) return 1;\n"
            "    if (expect_ok(mem_service_client_resolve_kv_segment(&client, &kv_selector, &record, &status), status, \"resolve_kv\") != 0) return 1;\n"
            "    if (strcmp(record.block_hash, \"typed-kv-block\") != 0) return fail(\"kv block mismatch\");\n"
            "    if (expect_ok(mem_service_client_publish_runtime_handoff(&client, &runtime_artifact, &record, &status), status, \"publish_runtime\") != 0) return 1;\n"
            "    if (expect_ok(mem_service_client_resolve_runtime_handoff(&client, &runtime_query, &record, &status), status, \"resolve_runtime\") != 0) return 1;\n"
            "    if (record.version != 3 || record.object_payload_checksum != 777) return fail(\"runtime artifact mismatch\");\n"
            "    if (expect_ok(mem_service_client_register_execution_artifact(&client, &execution_artifact, &record, &status), status, \"register_execution\") != 0) return 1;\n"
            "    if (expect_ok(mem_service_client_query_execution_artifact(&client, &execution_query, &record, &status), status, \"query_execution\") != 0) return 1;\n"
            "    if (record.version != 4 || record.object_payload_checksum != 888) return fail(\"execution artifact mismatch\");\n"
            "    if (expect_ok(mem_service_client_publish_checkpoint(&client, &training_ref, &record, &status), status, \"publish_checkpoint\") != 0) return 1;\n"
            "    if (expect_ok(mem_service_client_resolve_checkpoint(&client, &training_query, &record, &status), status, \"resolve_checkpoint\") != 0) return 1;\n"
            "    if (record.version != 5 || record.object_payload_checksum != 999) return fail(\"training artifact mismatch\");\n"
            "    if (expect_ok(mem_service_client_commit_training_step(&client, &step_commit, &record, &status), status, \"commit_training_step\") != 0) return 1;\n"
            "    if (expect_ok(mem_service_client_resolve_training_step(&client, &step_query, &record, &status), status, \"resolve_training_step\") != 0) return 1;\n"
            "    if (strcmp(record.artifact_kind, MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND) != 0 || record.version != 6 || record.object_payload_checksum != 1000) return fail(\"training step commit mismatch\");\n"
            "    printf(\"typed_client_roundtrip=ok\\n\");\n"
            "    return 0;\n"
            "}\n"
        )
        cmd = [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ROOT}",
            f"-I{ROOT.parent}",
            str(source),
            str(SERVICE_DIR / "mem_service_client.c"),
            str(SERVICE_DIR / "mem_service_wire_client.c"),
            "-o",
            str(self.client_binary),
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _compile_sdk_example(self, source_name: str, binary_name: str) -> Path:
        binary = self.root / binary_name
        cmd = [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{SERVICE_DIR}",
            str(SDK_EXAMPLES_DIR / source_name),
            str(SERVICE_DIR / "mem_service_client.c"),
            str(SERVICE_DIR / "mem_service_wire_client.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)
        return binary

    def _compile_pretraining_worker_binary(self) -> Path:
        source = self.root / "mem_service_pretraining_worker.c"
        binary = self.root / "mem_service_pretraining_worker"

        source.write_text(
            r'''
#include <stdio.h>
#include <string.h>

#include "components/mem_service/mem_service_client.h"

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
                "%s failed rc=%d status=%s\n",
                operation,
                rc,
                mem_service_wire_status_name(status));
        return 1;
    }
    return 0;
}

static int expect_status(int rc,
                         enum mem_service_wire_status status,
                         enum mem_service_wire_status expected,
                         const char *operation)
{
    if (rc == 0 || status != expected) {
        fprintf(stderr,
                "%s expected status=%s got rc=%d status=%s\n",
                operation,
                mem_service_wire_status_name(expected),
                rc,
                mem_service_wire_status_name(status));
        return 1;
    }
    return 0;
}

static int expect_record(const struct mem_service_client_record *record,
                         const char *key,
                         const char *artifact_kind,
                         uint64_t version,
                         uint64_t checksum)
{
    if (strcmp(record->key, key) != 0 ||
        strcmp(record->artifact_kind, artifact_kind) != 0 ||
        record->version != version ||
        record->object_payload_checksum != checksum) {
        fprintf(stderr,
                "record mismatch key=%s kind=%s version=%llu checksum=%llu\n",
                record->key,
                record->artifact_kind,
                (unsigned long long)record->version,
                (unsigned long long)record->object_payload_checksum);
        return 1;
    }
    return 0;
}

static int publish_dataset_shard(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref ref = {
        .key = "training/run-b/worker-0/dataset-shard-0000",
        .idempotency_key = "pretrain/run-b/w0/dataset/v1",
        .session_id = "run-b",
        .request_id = "global-step-10/worker-0",
        .model_key = "qwen3-14b-pretrain",
        .artifact_id = "dataset-shard-0000",
        .has_owner = true,
        .owner = 0,
        .has_payload_kind = true,
        .payload_kind = 10,
        .has_backing_offset = true,
        .backing_offset = 1000,
        .has_backing_len = true,
        .backing_len = 4096,
        .has_checksum = true,
        .checksum = 1001,
        .has_version = true,
        .version = 1,
    };

    if (expect_ok(mem_service_client_publish_dataset_shard(client,
                                                           &ref,
                                                           &record,
                                                           &status),
                  status,
                  "publish_dataset_shard") != 0) {
        return 1;
    }
    return expect_record(&record, ref.key, "dataset-shard", ref.version, ref.checksum);
}

static int publish_checkpoint(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref ref = {
        .key = "training/run-b/checkpoint-0010",
        .idempotency_key = "pretrain/run-b/w0/checkpoint/v4",
        .session_id = "run-b",
        .request_id = "global-step-10/worker-0",
        .model_key = "qwen3-14b-pretrain",
        .artifact_id = "checkpoint-0010",
        .has_owner = true,
        .owner = 0,
        .has_payload_kind = true,
        .payload_kind = 12,
        .has_backing_offset = true,
        .backing_offset = 8192,
        .has_backing_len = true,
        .backing_len = 65536,
        .has_checksum = true,
        .checksum = 4004,
        .has_version = true,
        .version = 4,
    };

    if (expect_ok(mem_service_client_publish_checkpoint(client,
                                                        &ref,
                                                        &record,
                                                        &status),
                  status,
                  "publish_checkpoint") != 0) {
        return 1;
    }
    return expect_record(&record, ref.key, "checkpoint", ref.version, ref.checksum);
}

static int publish_sample_batch(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref ref = {
        .key = "training/run-b/worker-1/sample-batch-0010",
        .idempotency_key = "pretrain/run-b/w1/sample/v2",
        .session_id = "run-b",
        .request_id = "global-step-10/worker-1",
        .model_key = "qwen3-14b-pretrain",
        .artifact_id = "sample-batch-0010",
        .has_owner = true,
        .owner = 1,
        .has_payload_kind = true,
        .payload_kind = 11,
        .has_backing_offset = true,
        .backing_offset = 2048,
        .has_backing_len = true,
        .backing_len = 8192,
        .has_checksum = true,
        .checksum = 2002,
        .has_version = true,
        .version = 2,
    };

    if (expect_ok(mem_service_client_publish_sample_batch(client,
                                                          &ref,
                                                          &record,
                                                          &status),
                  status,
                  "publish_sample_batch") != 0) {
        return 1;
    }
    return expect_record(&record, ref.key, "sample-batch", ref.version, ref.checksum);
}

static int publish_gradient_bucket(const struct mem_service_client *client,
                                   uint64_t checksum,
                                   enum mem_service_wire_status expected_status)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;
    struct mem_service_client_training_ref ref = {
        .key = "training/run-b/worker-1/gradient-bucket-0010",
        .idempotency_key = "pretrain/run-b/w1/gradient/v3",
        .session_id = "run-b",
        .request_id = "global-step-10/worker-1",
        .model_key = "qwen3-14b-pretrain",
        .artifact_id = "gradient-bucket-0010",
        .has_owner = true,
        .owner = 1,
        .has_payload_kind = true,
        .payload_kind = 13,
        .has_backing_offset = true,
        .backing_offset = 16384,
        .has_backing_len = true,
        .backing_len = 32768,
        .has_checksum = true,
        .checksum = checksum,
        .has_version = true,
        .version = 3,
    };

    rc = mem_service_client_publish_gradient_bucket(client, &ref, &record, &status);
    if (expected_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return expect_status(rc, status, expected_status, "publish_gradient_bucket");
    }
    if (expect_ok(rc, status, "publish_gradient_bucket") != 0) {
        return 1;
    }
    return expect_record(&record, ref.key, "gradient-bucket", ref.version, checksum);
}

static int publish_optimizer_state(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref ref = {
        .key = "training/run-b/worker-1/optimizer-state-0010",
        .idempotency_key = "pretrain/run-b/w1/optimizer/v5",
        .session_id = "run-b",
        .request_id = "global-step-10/worker-1",
        .model_key = "qwen3-14b-pretrain",
        .artifact_id = "optimizer-state-0010",
        .has_owner = true,
        .owner = 1,
        .has_payload_kind = true,
        .payload_kind = 14,
        .has_backing_offset = true,
        .backing_offset = 49152,
        .has_backing_len = true,
        .backing_len = 32768,
        .has_checksum = true,
        .checksum = 5005,
        .has_version = true,
        .version = 5,
    };

    if (expect_ok(mem_service_client_publish_optimizer_state(client,
                                                             &ref,
                                                             &record,
                                                             &status),
                  status,
                  "publish_optimizer_state") != 0) {
        return 1;
    }
    return expect_record(&record,
                         ref.key,
                         "optimizer-state",
                         ref.version,
                         ref.checksum);
}

static int commit_training_step(const struct mem_service_client *client,
                                uint64_t checksum,
                                enum mem_service_wire_status expected_status)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;
    struct mem_service_client_training_ref ref = {
        .key = "training/run-b/global-step-0010/commit",
        .idempotency_key = "pretrain/run-b/global-step-10/commit/v6",
        .session_id = "run-b",
        .request_id = "global-step-10",
        .model_key = "qwen3-14b-pretrain",
        .artifact_id = "global-step-0010",
        .has_owner = true,
        .owner = 0,
        .has_payload_kind = true,
        .payload_kind = 15,
        .has_backing_offset = true,
        .backing_offset = 81920,
        .has_backing_len = true,
        .backing_len = 64,
        .has_checksum = true,
        .checksum = checksum,
        .has_version = true,
        .version = 6,
    };

    rc = mem_service_client_commit_training_step(client, &ref, &record, &status);
    if (expected_status != MEM_SERVICE_WIRE_STATUS_OK) {
        return expect_status(rc, status, expected_status, "commit_training_step");
    }
    if (expect_ok(rc, status, "commit_training_step") != 0) {
        return 1;
    }
    return expect_record(&record,
                         ref.key,
                         MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND,
                         ref.version,
                         checksum);
}

static int resolve_training_ref(const struct mem_service_client *client,
                                const char *key,
                                const char *artifact_id,
                                const char *artifact_kind,
                                uint64_t version,
                                uint64_t checksum,
                                int (*resolve_fn)(
                                    const struct mem_service_client *,
                                    const struct mem_service_client_training_ref_query *,
                                    struct mem_service_client_record *,
                                    enum mem_service_wire_status *))
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref_query query = {
        .key = key,
        .expected_session_id = "run-b",
        .expected_model_key = "qwen3-14b-pretrain",
        .expected_artifact_id = artifact_id,
        .has_expected_version = true,
        .expected_version = version,
        .has_expected_checksum = true,
        .expected_checksum = checksum,
    };

    if (expect_ok(resolve_fn(client, &query, &record, &status),
                  status,
                  artifact_kind) != 0) {
        return 1;
    }
    return expect_record(&record, key, artifact_kind, version, checksum);
}

static int resolve_all(const struct mem_service_client *client)
{
    if (resolve_training_ref(client,
                             "training/run-b/worker-0/dataset-shard-0000",
                             "dataset-shard-0000",
                             "dataset-shard",
                             1,
                             1001,
                             mem_service_client_resolve_dataset_shard) != 0 ||
        resolve_training_ref(client,
                             "training/run-b/worker-1/sample-batch-0010",
                             "sample-batch-0010",
                             "sample-batch",
                             2,
                             2002,
                             mem_service_client_resolve_sample_batch) != 0 ||
        resolve_training_ref(client,
                             "training/run-b/worker-1/gradient-bucket-0010",
                             "gradient-bucket-0010",
                             "gradient-bucket",
                             3,
                             3003,
                             mem_service_client_resolve_gradient_bucket) != 0 ||
        resolve_training_ref(client,
                             "training/run-b/checkpoint-0010",
                             "checkpoint-0010",
                             "checkpoint",
                             4,
                             4004,
                             mem_service_client_resolve_checkpoint) != 0 ||
        resolve_training_ref(client,
                             "training/run-b/worker-1/optimizer-state-0010",
                             "optimizer-state-0010",
                             "optimizer-state",
                             5,
                             5005,
                             mem_service_client_resolve_optimizer_state) != 0 ||
        resolve_training_ref(client,
                             "training/run-b/global-step-0010/commit",
                             "global-step-0010",
                             MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND,
                             6,
                             6006,
                             mem_service_client_resolve_training_step) != 0) {
        return 1;
    }
    return 0;
}

static int resolve_step_bad_version(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref_query query = {
        .key = "training/run-b/global-step-0010/commit",
        .expected_session_id = "run-b",
        .expected_model_key = "qwen3-14b-pretrain",
        .expected_artifact_id = "global-step-0010",
        .has_expected_version = true,
        .expected_version = 99,
        .has_expected_checksum = true,
        .expected_checksum = 6006,
    };

    return expect_status(mem_service_client_resolve_training_step(client,
                                                                  &query,
                                                                  &record,
                                                                  &status),
                         status,
                         MEM_SERVICE_WIRE_STATUS_STALE_REF,
                         "resolve_step_bad_version");
}

static int resolve_step_bad_checksum(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref_query query = {
        .key = "training/run-b/global-step-0010/commit",
        .expected_session_id = "run-b",
        .expected_model_key = "qwen3-14b-pretrain",
        .expected_artifact_id = "global-step-0010",
        .has_expected_version = true,
        .expected_version = 6,
        .has_expected_checksum = true,
        .expected_checksum = 6666,
    };

    return expect_status(mem_service_client_resolve_training_step(client,
                                                                  &query,
                                                                  &record,
                                                                  &status),
                         status,
                         MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH,
                         "resolve_step_bad_checksum");
}

static int resolve_bad_version(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref_query query = {
        .key = "training/run-b/checkpoint-0010",
        .expected_session_id = "run-b",
        .expected_model_key = "qwen3-14b-pretrain",
        .expected_artifact_id = "checkpoint-0010",
        .has_expected_version = true,
        .expected_version = 99,
        .has_expected_checksum = true,
        .expected_checksum = 4004,
    };

    return expect_status(mem_service_client_resolve_checkpoint(client,
                                                               &query,
                                                               &record,
                                                               &status),
                         status,
                         MEM_SERVICE_WIRE_STATUS_STALE_REF,
                         "resolve_bad_version");
}

static int resolve_bad_checksum(const struct mem_service_client *client)
{
    struct mem_service_client_record record;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_training_ref_query query = {
        .key = "training/run-b/worker-1/gradient-bucket-0010",
        .expected_session_id = "run-b",
        .expected_model_key = "qwen3-14b-pretrain",
        .expected_artifact_id = "gradient-bucket-0010",
        .has_expected_version = true,
        .expected_version = 3,
        .has_expected_checksum = true,
        .expected_checksum = 3333,
    };

    return expect_status(mem_service_client_resolve_gradient_bucket(client,
                                                                    &query,
                                                                    &record,
                                                                    &status),
                         status,
                         MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH,
                         "resolve_bad_checksum");
}

int main(int argc, char **argv)
{
    struct mem_service_client client;
    struct mem_service_wire_client_options options;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    const char *mode;

    if (argc != 3) {
        return fail("usage: mem_service_pretraining_worker unix:/path/to.sock MODE");
    }
    mode = argv[2];
    mem_service_wire_client_options_init(&options);
    options.timeout_ms = 2000;
    options.max_attempts = 3;
    options.retry_backoff_ms = 5;
    options.retry_on_timeout = 1;
    mem_service_client_init_with_options(&client, argv[1], &options);
    if (expect_ok(mem_service_client_health(&client, &status), status, "health") != 0 ||
        expect_ok(mem_service_client_ready(&client, &status), status, "ready") != 0) {
        return 1;
    }
    if (strcmp(mode, "worker0") == 0) {
        if (publish_dataset_shard(&client) != 0 ||
            publish_checkpoint(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "worker1") == 0) {
        if (publish_sample_batch(&client) != 0 ||
            publish_gradient_bucket(&client, 3003, MEM_SERVICE_WIRE_STATUS_OK) != 0 ||
            publish_optimizer_state(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "commit-step") == 0) {
        if (commit_training_step(&client, 6006, MEM_SERVICE_WIRE_STATUS_OK) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "resolve") == 0) {
        if (resolve_all(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "bad-version") == 0) {
        if (resolve_bad_version(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "bad-checksum") == 0) {
        if (resolve_bad_checksum(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "step-bad-version") == 0) {
        if (resolve_step_bad_version(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "step-bad-checksum") == 0) {
        if (resolve_step_bad_checksum(&client) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "conflict") == 0) {
        if (publish_gradient_bucket(&client,
                                    3333,
                                    MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT) != 0 ||
            resolve_training_ref(&client,
                                 "training/run-b/worker-1/gradient-bucket-0010",
                                 "gradient-bucket-0010",
                                 "gradient-bucket",
                                 3,
                                 3003,
                                 mem_service_client_resolve_gradient_bucket) != 0) {
            return 1;
        }
    } else if (strcmp(mode, "step-conflict") == 0) {
        if (commit_training_step(&client,
                                 6666,
                                 MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT) != 0 ||
            resolve_training_ref(&client,
                                 "training/run-b/global-step-0010/commit",
                                 "global-step-0010",
                                 MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND,
                                 6,
                                 6006,
                                 mem_service_client_resolve_training_step) != 0) {
            return 1;
        }
    } else {
        return fail("unknown mode");
    }
    printf("pretraining_worker=%s ok\n", mode);
    return 0;
}
'''
        )
        cmd = [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ROOT}",
            f"-I{ROOT.parent}",
            str(source),
            str(SERVICE_DIR / "mem_service_client.c"),
            str(SERVICE_DIR / "mem_service_wire_client.c"),
            "-o",
            str(binary),
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)
        return binary

    def _run_pretraining_worker(self, binary: Path, mode: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(binary), f"unix:{self.socket}", mode],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

    def _run_client(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.binary), *args],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

    def _parse_metrics(self, payload: str) -> dict[str, int]:
        metrics: dict[str, int] = {}

        for line in payload.splitlines():
            if "=" not in line:
                continue
            name, value = line.split("=", 1)
            if value.isdigit():
                metrics[name] = int(value)
        return metrics

    def _free_tcp_port(self) -> int:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listener.bind(("127.0.0.1", 0))
            return int(listener.getsockname()[1])
        finally:
            listener.close()

    def _start_server(
        self,
        metrics_port: int | None = None,
        config_path: Path | None = None,
    ) -> subprocess.Popen:
        if config_path is not None:
            cmd = [str(self.binary), "serve", "--config", str(config_path)]
        else:
            cmd = [
                str(self.binary),
                "serve",
                "--listen",
                f"unix:{self.socket}",
                "--store",
                str(self.store),
            ]
            if metrics_port is not None:
                cmd.extend(["--metrics-listen", f"tcp:127.0.0.1:{metrics_port}"])
        proc = subprocess.Popen(
            cmd,
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate(timeout=1)
                if "Operation not permitted" in stderr and "mem_service serve: bind" in stderr:
                    raise unittest.SkipTest("sandbox forbids Unix socket bind in subprocess")
                if (
                    "Operation not permitted" in stderr
                    and "mem_service serve: metrics bind" in stderr
                ):
                    raise unittest.SkipTest("sandbox forbids TCP metrics bind in subprocess")
                self.fail(
                    f"mem_service daemon exited rc={proc.returncode}\nstdout={stdout}\nstderr={stderr}"
                )
            health = self._run_client("health", "--connect", f"unix:{self.socket}")
            if health.returncode == 0 and "status=ok" in health.stdout:
                return proc
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service daemon did not become ready")

    def test_daemon_rejects_non_loopback_metrics_listener(self):
        metrics_port = self._free_tcp_port()
        result = subprocess.run(
            [
                str(self.binary),
                "serve",
                "--listen",
                f"unix:{self.socket}",
                "--store",
                str(self.store),
                "--metrics-listen",
                f"tcp:0.0.0.0:{metrics_port}",
            ],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("invalid metrics listen path", result.stderr)

    def _http_metrics_request(
        self,
        port: int,
        method: str = "GET",
        path: str = "/metrics",
    ) -> str:
        request = (
            f"{method} {path} HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).encode()
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as conn:
            conn.sendall(request)
            chunks = []
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
        return b"".join(chunks).decode("utf-8", errors="replace")

    def _stop_server(self, proc: subprocess.Popen):
        try:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
        finally:
            if proc.stdout is not None:
                proc.stdout.close()
            if proc.stderr is not None:
                proc.stderr.close()

    def _stop_server_and_collect(self, proc: subprocess.Popen) -> tuple[str, str, int]:
        if proc.poll() is None:
            proc.terminate()
            try:
                stdout, stderr = proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate(timeout=3)
        else:
            stdout, stderr = proc.communicate(timeout=1)
        return stdout, stderr, proc.returncode

    def test_client_timeout_option_fails_closed_against_silent_peer(self):
        silent_socket = self.root / "silent.sock"
        ready = threading.Event()
        done = threading.Event()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

        def accept_without_response() -> None:
            ready.set()
            conn, _ = server.accept()
            with conn:
                time.sleep(0.2)
            done.set()

        try:
            server.bind(str(silent_socket))
        except PermissionError as exc:
            server.close()
            raise unittest.SkipTest("sandbox forbids Unix socket bind in test") from exc
        server.listen(1)
        thread = threading.Thread(target=accept_without_response, daemon=True)
        thread.start()
        try:
            self.assertTrue(ready.wait(timeout=1))
            health = self._run_client(
                "health",
                "--connect",
                f"unix:{silent_socket}",
                "--timeout-ms",
                "1",
            )
            self.assertNotEqual(health.returncode, 0, health.stdout)
            self.assertIn("status=timeout", health.stdout)
            self.assertIn("timeout", health.stderr)
        finally:
            server.close()
            done.wait(timeout=1)

    def test_idempotency_key_replays_mutation_and_rejects_conflict(self):
        proc = self._start_server()
        publish_args = [
            "publish-runtime-handoff",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "runtime/idempotent/range-0",
            "--session-id",
            "session-idempotent",
            "--model-key",
            "model-idempotent",
            "--artifact-kind",
            "hidden-range",
            "--artifact-id",
            "range-0",
            "--idempotency-key",
            "idem-runtime-0",
        ]
        try:
            first = self._run_client(*publish_args)
            self.assertEqual(first.returncode, 0, first.stderr + first.stdout)
            self.assertIn("status=ok", first.stdout)
            self.assertIn("version=1", first.stdout)

            replay = self._run_client(*publish_args)
            self.assertEqual(replay.returncode, 0, replay.stderr + replay.stdout)
            self.assertIn("status=ok", replay.stdout)
            self.assertIn("version=1", replay.stdout)

            conflict = self._run_client(*publish_args, "--checksum", "777")
            self.assertNotEqual(conflict.returncode, 0, conflict.stdout)
            self.assertIn("status=version_conflict", conflict.stdout)

            resolved = self._run_client(
                "resolve-runtime-handoff",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "runtime/idempotent/range-0",
                "--expected-session-id",
                "session-idempotent",
                "--expected-model-key",
                "model-idempotent",
                "--expected-artifact-kind",
                "hidden-range",
                "--expected-artifact-id",
                "range-0",
                "--expected-version",
                "1",
            )
            self.assertEqual(resolved.returncode, 0, resolved.stderr + resolved.stdout)
            self.assertIn("status=ok", resolved.stdout)
            self.assertIn("version=1", resolved.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            parsed_metrics = self._parse_metrics(metrics.stdout)
            self.assertEqual(parsed_metrics["idempotency_replay_count"], 1)
            self.assertEqual(parsed_metrics["idempotency_conflict_count"], 1)
            self.assertEqual(parsed_metrics["fail_closed_count"], 1)
        finally:
            self._stop_server(proc)

    def test_cli_training_step_commit_barrier_round_trips_fail_closed(self):
        proc = self._start_server()
        commit_args = [
            "commit-training-step",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "training/cli-run/global-step-0002/commit",
            "--session-id",
            "cli-run",
            "--request-id",
            "global-step-2",
            "--model-key",
            "qwen3-14b-pretrain",
            "--artifact-id",
            "global-step-0002",
            "--backing-len",
            "64",
            "--checksum",
            "2222",
            "--version",
            "2",
            "--idempotency-key",
            "pretrain/cli-run/global-step-2/commit/v2",
        ]
        resolve_args = [
            "resolve-training-step",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "training/cli-run/global-step-0002/commit",
            "--expected-session-id",
            "cli-run",
            "--expected-model-key",
            "qwen3-14b-pretrain",
            "--expected-artifact-id",
            "global-step-0002",
            "--expected-version",
            "2",
            "--expected-checksum",
            "2222",
        ]
        stale_args = [
            "resolve-training-step",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "training/cli-run/global-step-0002/commit",
            "--expected-session-id",
            "cli-run",
            "--expected-model-key",
            "qwen3-14b-pretrain",
            "--expected-artifact-id",
            "global-step-0002",
            "--expected-version",
            "3",
            "--expected-checksum",
            "2222",
        ]
        conflict_args = [
            "commit-training-step",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "training/cli-run/global-step-0002/commit",
            "--session-id",
            "cli-run",
            "--request-id",
            "global-step-2",
            "--model-key",
            "qwen3-14b-pretrain",
            "--artifact-id",
            "global-step-0002",
            "--backing-len",
            "64",
            "--checksum",
            "3333",
            "--version",
            "2",
            "--idempotency-key",
            "pretrain/cli-run/global-step-2/commit/v2",
        ]
        try:
            committed = self._run_client(*commit_args)
            self.assertEqual(committed.returncode, 0, committed.stderr + committed.stdout)
            self.assertIn("status=ok", committed.stdout)
            self.assertIn("artifact_kind=training-step-commit", committed.stdout)
            self.assertIn("version=2", committed.stdout)
            self.assertIn("object_payload_checksum=2222", committed.stdout)

            resolved = self._run_client(*resolve_args)
            self.assertEqual(resolved.returncode, 0, resolved.stderr + resolved.stdout)
            self.assertIn("status=ok", resolved.stdout)
            self.assertIn("artifact_kind=training-step-commit", resolved.stdout)

            stale = self._run_client(*stale_args)
            self.assertNotEqual(stale.returncode, 0, stale.stdout)
            self.assertIn("status=stale_ref", stale.stdout)

            conflict = self._run_client(*conflict_args)
            self.assertNotEqual(conflict.returncode, 0, conflict.stdout)
            self.assertIn("status=version_conflict", conflict.stdout)
        finally:
            self._stop_server(proc)

    def test_audit_log_tracks_training_step_commit_and_fail_closed_after_restart(self):
        first = self._start_server()
        commit_args = [
            "commit-training-step",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "training/audit-run/global-step-0003/commit",
            "--session-id",
            "audit-run",
            "--request-id",
            "global-step-3",
            "--model-key",
            "qwen3-14b-pretrain",
            "--artifact-id",
            "global-step-0003",
            "--checksum",
            "3003",
            "--version",
            "3",
            "--idempotency-key",
            "pretrain/audit-run/global-step-3/commit/v3",
        ]
        stale_args = [
            "resolve-training-step",
            "--connect",
            f"unix:{self.socket}",
            "--key",
            "training/audit-run/global-step-0003/commit",
            "--expected-session-id",
            "audit-run",
            "--expected-model-key",
            "qwen3-14b-pretrain",
            "--expected-artifact-id",
            "global-step-0003",
            "--expected-version",
            "4",
            "--expected-checksum",
            "3003",
        ]
        audit_args = [
            "audit-log",
            "--connect",
            f"unix:{self.socket}",
            "--start-sequence",
            "1",
            "--max-events",
            "8",
        ]
        try:
            committed = self._run_client(*commit_args)
            self.assertEqual(committed.returncode, 0, committed.stderr + committed.stdout)
            self.assertIn("status=ok", committed.stdout)

            stale = self._run_client(*stale_args)
            self.assertNotEqual(stale.returncode, 0, stale.stdout)
            self.assertIn("status=stale_ref", stale.stdout)

            audit = self._run_client(*audit_args)
            self.assertEqual(audit.returncode, 0, audit.stderr + audit.stdout)
            self.assertIn("status=ok", audit.stdout)
            self.assertIn("audit_log=1", audit.stdout)
            self.assertIn("operation_name=register_training_artifact", audit.stdout)
            self.assertIn("operation_name=query_training_artifact", audit.stdout)
            self.assertIn("status_name=ok", audit.stdout)
            self.assertIn("status_name=stale_ref", audit.stdout)
            self.assertIn("artifact_kind=training-step-commit", audit.stdout)
            self.assertIn("artifact_id=global-step-0003", audit.stdout)
            self.assertIn("idempotency_key=pretrain/audit-run/global-step-3/commit/v3",
                          audit.stdout)
            self.assertIn("events_emitted=2", audit.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            parsed_metrics = self._parse_metrics(metrics.stdout)
            self.assertEqual(parsed_metrics["audit_log_count"], 1)
            self.assertEqual(parsed_metrics["stale_ref_count"], 1)
            self.assertEqual(parsed_metrics["fail_closed_count"], 1)
        finally:
            self._stop_server(first)

        store_text = self.store.read_text()
        self.assertIn("audit_begin", store_text)
        self.assertIn("operation=96", store_text)
        self.assertIn("operation=97", store_text)
        self.assertIn("status=0", store_text)
        self.assertIn("status=2", store_text)
        self.assertIn("artifact_kind=training-step-commit", store_text)
        self.assertIn("artifact_id=global-step-0003", store_text)

        second = self._start_server()
        try:
            audit_after_restart = self._run_client(*audit_args)
            self.assertEqual(audit_after_restart.returncode,
                             0,
                             audit_after_restart.stderr + audit_after_restart.stdout)
            self.assertIn("status=ok", audit_after_restart.stdout)
            self.assertIn("events_emitted=2", audit_after_restart.stdout)
            self.assertIn("operation_name=register_training_artifact",
                          audit_after_restart.stdout)
            self.assertIn("operation_name=query_training_artifact",
                          audit_after_restart.stdout)
            self.assertIn("artifact_kind=training-step-commit",
                          audit_after_restart.stdout)
        finally:
            self._stop_server(second)

    def test_daemon_store_survives_restart_for_object_refs(self):
        first = self._start_server()
        try:
            put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "durable-object",
                "--version",
                "7",
                "--checksum",
                "12345",
                "--backing-len",
                "64",
                "--idempotency-key",
                "idem-durable-object",
            )
            self.assertEqual(put.returncode, 0, put.stderr + put.stdout)
            self.assertIn("status=ok", put.stdout)
            put_second = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "durable-object-2",
                "--version",
                "8",
                "--checksum",
                "54321",
                "--backing-len",
                "128",
            )
            self.assertEqual(put_second.returncode, 0, put_second.stderr + put_second.stdout)
            self.assertIn("status=ok", put_second.stdout)
            status = self._run_client("status", "--connect", f"unix:{self.socket}")
            self.assertEqual(status.returncode, 0, status.stderr + status.stdout)
            self.assertIn("record_count=2", status.stdout)
            self.assertIn("object_count=2", status.stdout)
            records = self._run_client("list-records", "--connect", f"unix:{self.socket}")
            self.assertEqual(records.returncode, 0, records.stderr + records.stdout)
            self.assertIn("kind_name=kvcache_object", records.stdout)
            self.assertIn("key=durable-object", records.stdout)
            self.assertIn("key=durable-object-2", records.stdout)
            inspect = self._run_client(
                "inspect-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "durable-object",
            )
            self.assertEqual(inspect.returncode, 0, inspect.stderr + inspect.stdout)
            self.assertIn("status=ok", inspect.stdout)
            self.assertIn("kind_name=kvcache_object", inspect.stdout)
            self.assertIn("object_payload_checksum=12345", inspect.stdout)
            exported = self._run_client("export-snapshot", "--connect", f"unix:{self.socket}")
            self.assertEqual(exported.returncode, 0, exported.stderr + exported.stdout)
            self.assertIn("status=ok", exported.stdout)
            self.assertIn("mem_service_store_v1", exported.stdout)
            self.assertIn("key=durable-object", exported.stdout)
            self.assertIn("key=durable-object-2", exported.stdout)
            self.assertIn("object_payload_checksum=12345", exported.stdout)
            first_page = self._run_client(
                "export-snapshot-page",
                "--connect",
                f"unix:{self.socket}",
                "--start-index",
                "0",
                "--max-records",
                "1",
            )
            self.assertEqual(first_page.returncode, 0, first_page.stderr + first_page.stdout)
            self.assertIn("status=ok", first_page.stdout)
            self.assertIn("snapshot_page=1", first_page.stdout)
            self.assertIn("records_emitted=1", first_page.stdout)
            self.assertIn("complete=0", first_page.stdout)
            self.assertIn("next_index=1", first_page.stdout)
            self.assertIn("key=durable-object", first_page.stdout)
            self.assertNotIn("key=durable-object-2", first_page.stdout)
            second_page = self._run_client(
                "export-snapshot-page",
                "--connect",
                f"unix:{self.socket}",
                "--start-index",
                "1",
                "--max-records",
                "1",
            )
            self.assertEqual(second_page.returncode, 0, second_page.stderr + second_page.stdout)
            self.assertIn("status=ok", second_page.stdout)
            self.assertIn("records_emitted=1", second_page.stdout)
            self.assertIn("complete=1", second_page.stdout)
            self.assertIn("key=durable-object-2", second_page.stdout)

            snapshot_text = exported.stdout.split("\n", 1)[1]
            snapshot_path = self.root / "restore.snapshot"
            snapshot_path.write_text(snapshot_text)
            invalid_snapshot = self.root / "invalid.snapshot"
            invalid_snapshot.write_text("not_mem_service_store\n")
            temp_put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "temp-object",
                "--version",
                "9",
                "--checksum",
                "999",
                "--backing-len",
                "256",
            )
            self.assertEqual(temp_put.returncode, 0, temp_put.stderr + temp_put.stdout)
            self.assertIn("status=ok", temp_put.stdout)
            bad_restore = self._run_client(
                "restore-snapshot",
                "--connect",
                f"unix:{self.socket}",
                "--from",
                str(invalid_snapshot),
            )
            self.assertNotEqual(bad_restore.returncode, 0, bad_restore.stdout)
            self.assertIn("status=invalid_session", bad_restore.stdout)
            temp_get = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "temp-object",
            )
            self.assertEqual(temp_get.returncode, 0, temp_get.stderr + temp_get.stdout)
            self.assertIn("status=ok", temp_get.stdout)
            restored = self._run_client(
                "restore-snapshot",
                str(snapshot_path),
                "--connect",
                f"unix:{self.socket}",
            )
            self.assertEqual(restored.returncode, 0, restored.stderr + restored.stdout)
            self.assertIn("status=ok", restored.stdout)
            self.assertIn("restored=1", restored.stdout)
            self.assertIn("record_count=2", restored.stdout)
            missing_temp = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "temp-object",
            )
            self.assertNotEqual(missing_temp.returncode, 0, missing_temp.stdout)
            self.assertIn("status=not_found", missing_temp.stdout)
        finally:
            self._stop_server(first)

        store_text = self.store.read_text()
        self.assertIn("mem_service_store_v1", store_text)
        self.assertIn("key=durable-object", store_text)
        self.assertIn("key=durable-object-2", store_text)
        self.assertNotIn("key=temp-object", store_text)
        self.assertIn("object_payload_checksum=12345", store_text)
        self.assertIn("idempotency_begin", store_text)
        self.assertIn("key=idem-durable-object", store_text)
        self.assertIn("operation=16", store_text)
        self.assertIn("response_line=status=ok", store_text)

        second = self._start_server()
        try:
            replay_put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "durable-object",
                "--version",
                "7",
                "--checksum",
                "12345",
                "--backing-len",
                "64",
                "--idempotency-key",
                "idem-durable-object",
            )
            self.assertEqual(replay_put.returncode, 0, replay_put.stderr + replay_put.stdout)
            self.assertIn("status=ok", replay_put.stdout)
            self.assertIn("version=7", replay_put.stdout)

            conflict_put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "durable-object",
                "--version",
                "7",
                "--checksum",
                "77777",
                "--backing-len",
                "64",
                "--idempotency-key",
                "idem-durable-object",
            )
            self.assertNotEqual(conflict_put.returncode, 0, conflict_put.stdout)
            self.assertIn("status=version_conflict", conflict_put.stdout)

            get = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "durable-object",
            )
            self.assertEqual(get.returncode, 0, get.stderr + get.stdout)
            self.assertIn("status=ok", get.stdout)
            self.assertIn("version=7", get.stdout)
            self.assertIn("object_backing_len=64", get.stdout)
            self.assertIn("object_payload_checksum=12345", get.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            parsed_metrics = self._parse_metrics(metrics.stdout)
            self.assertEqual(parsed_metrics["idempotency_replay_count"], 1)
            self.assertEqual(parsed_metrics["idempotency_conflict_count"], 1)
        finally:
            self._stop_server(second)

    def test_export_snapshot_to_and_restore_large_snapshot_file(self):
        proc = self._start_server()
        snapshot = self.root / "large.snapshot"
        try:
            for index in range(32):
                put = self._run_client(
                    "put-object",
                    "--connect",
                    f"unix:{self.socket}",
                    "--key",
                    f"large-object-{index:02d}",
                    "--version",
                    str(100 + index),
                    "--checksum",
                    str(1000 + index),
                    "--backing-len",
                    str(4096 + index),
                )
                self.assertEqual(put.returncode, 0, put.stderr + put.stdout)
                self.assertIn("status=ok", put.stdout)

            full_export = self._run_client(
                "export-snapshot",
                "--connect",
                f"unix:{self.socket}",
            )
            self.assertNotEqual(full_export.returncode, 0, full_export.stdout)
            self.assertIn("status=capacity_exceeded", full_export.stdout)

            export_to = self._run_client(
                "export-snapshot-to",
                "--connect",
                f"unix:{self.socket}",
                "--to",
                str(snapshot),
                "--max-records",
                "3",
            )
            self.assertEqual(export_to.returncode, 0, export_to.stderr + export_to.stdout)
            self.assertIn("status=ok", export_to.stdout)
            self.assertIn("record_count=32", export_to.stdout)
            snapshot_text = snapshot.read_text()
            self.assertGreater(len(snapshot_text), 4096)
            self.assertIn("mem_service_store_v1", snapshot_text)
            self.assertIn("record_count=32", snapshot_text)
            self.assertIn("key=large-object-00", snapshot_text)
            self.assertIn("key=large-object-31", snapshot_text)

            transient = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "transient-object",
                "--version",
                "999",
                "--checksum",
                "9999",
                "--backing-len",
                "999",
            )
            self.assertEqual(transient.returncode, 0, transient.stderr + transient.stdout)

            restored = self._run_client(
                "restore-snapshot",
                "--connect",
                f"unix:{self.socket}",
                "--from",
                str(snapshot),
            )
            self.assertEqual(restored.returncode, 0, restored.stderr + restored.stdout)
            self.assertIn("status=ok", restored.stdout)
            self.assertIn("restored=1", restored.stdout)
            self.assertIn("record_count=32", restored.stdout)

            missing = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "transient-object",
            )
            self.assertNotEqual(missing.returncode, 0, missing.stdout)
            self.assertIn("status=not_found", missing.stdout)

            restored_object = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "large-object-31",
            )
            self.assertEqual(restored_object.returncode,
                             0,
                             restored_object.stderr + restored_object.stdout)
            self.assertIn("status=ok", restored_object.stdout)
            self.assertIn("version=131", restored_object.stdout)
            self.assertIn("object_payload_checksum=1031", restored_object.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            self.assertIn("export_snapshot_page_count=", metrics.stdout)
            self.assertIn("restore_snapshot_page_count=", metrics.stdout)
            self.assertIn("request_latency_total_ms=", metrics.stdout)
            self.assertIn("request_latency_max_ms=", metrics.stdout)
            self.assertIn("request_latency_le_1ms_count=", metrics.stdout)
            self.assertIn("request_latency_gt_100ms_count=", metrics.stdout)
            parsed_metrics = self._parse_metrics(metrics.stdout)
            bucket_total = (
                parsed_metrics["request_latency_le_1ms_count"]
                + parsed_metrics["request_latency_le_5ms_count"]
                + parsed_metrics["request_latency_le_10ms_count"]
                + parsed_metrics["request_latency_le_50ms_count"]
                + parsed_metrics["request_latency_le_100ms_count"]
                + parsed_metrics["request_latency_gt_100ms_count"]
            )
            self.assertEqual(bucket_total, parsed_metrics["request_count"])
        finally:
            self._stop_server(proc)

    def test_release_manifest_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("release-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("public_headers=8", fixtures.stdout)
        self.assertIn("client_sources=2", fixtures.stdout)
        self.assertIn("examples=2", fixtures.stdout)
        self.assertIn("compat_artifacts=3", fixtures.stdout)
        self.assertIn("operations=23", fixtures.stdout)
        self.assertIn("schema_manifest_len=9416", fixtures.stdout)
        self.assertIn("schema_manifest_checksum=0xf4cf34c6", fixtures.stdout)
        self.assertIn("durable_backends=1", fixtures.stdout)
        self.assertIn("durable_catalogs=1", fixtures.stdout)
        self.assertIn("payload_block_backends=4", fixtures.stdout)
        self.assertIn("host_artifacts=1", fixtures.stdout)
        self.assertIn("systemd_units=2", fixtures.stdout)
        self.assertIn("package_artifacts=4", fixtures.stdout)
        self.assertIn("installed_sdk_runtime_smokes=1", fixtures.stdout)
        self.assertIn("deployment_smokes=1", fixtures.stdout)
        self.assertIn("service_manager_lifecycle_smokes=1", fixtures.stdout)
        self.assertIn("host_service_manager_smokes=1", fixtures.stdout)
        self.assertIn("collector_smokes=1", fixtures.stdout)
        self.assertIn("alert_rule_artifacts=1", fixtures.stdout)
        self.assertIn("alert_rules=6", fixtures.stdout)
        self.assertIn("alert_integration_smokes=1", fixtures.stdout)
        self.assertIn("api_abi_policies=1", fixtures.stdout)
        self.assertIn("admin_output_schemas=1", fixtures.stdout)
        self.assertIn("upgrade_rollback_policies=1", fixtures.stdout)
        self.assertIn("upgrade_rollback_runtime_smokes=1", fixtures.stdout)
        self.assertIn("api_abi_policy_len=856", fixtures.stdout)
        self.assertIn("api_abi_policy_checksum=0x5d95ae02", fixtures.stdout)
        self.assertIn("admin_output_schema_len=6624", fixtures.stdout)
        self.assertIn("admin_output_schema_checksum=0x7021f4cf", fixtures.stdout)
        self.assertIn("upgrade_rollback_policy_len=2019", fixtures.stdout)
        self.assertIn("upgrade_rollback_policy_checksum=0xf7943816", fixtures.stdout)
        self.assertIn("alert_rules_len=2096", fixtures.stdout)
        self.assertIn("alert_rules_checksum=0x05a9245c", fixtures.stdout)
        self.assertIn("ops_certification_policies=1", fixtures.stdout)
        self.assertIn("remote_transport_evidence_schemas=1", fixtures.stdout)
        self.assertIn("ops_certification_policy_len=1118", fixtures.stdout)
        self.assertIn("ops_certification_policy_checksum=0xe77c644b", fixtures.stdout)
        self.assertIn("version_smokes=1", fixtures.stdout)
        self.assertIn("release_readiness_smokes=1", fixtures.stdout)
        self.assertIn("restore_policy_smokes=1", fixtures.stdout)
        self.assertIn("config_security_smokes=1", fixtures.stdout)
        self.assertIn("retention_smokes=1", fixtures.stdout)
        self.assertIn("payload_gc_smokes=1", fixtures.stdout)
        self.assertIn("record_retention_smokes=1", fixtures.stdout)
        self.assertIn("package_manifest_len=9126", fixtures.stdout)
        self.assertIn("package_manifest_checksum=0x28945f1f", fixtures.stdout)
        self.assertIn("metrics_http_listeners=1", fixtures.stdout)
        self.assertIn("metrics_scrape_paths=1", fixtures.stdout)
        self.assertIn("compat_runtime_smokes=1", fixtures.stdout)
        self.assertIn("compat_matrix_len=1978", fixtures.stdout)
        self.assertIn("compat_matrix_checksum=0x61d07124", fixtures.stdout)
        self.assertIn("compat_baseline_len=1251", fixtures.stdout)
        self.assertIn("compat_baseline_checksum=0x1e017705", fixtures.stdout)
        self.assertIn("compat_old_new_matrix_len=1733", fixtures.stdout)
        self.assertIn("compat_old_new_matrix_checksum=0x627bf6a1", fixtures.stdout)

        manifest = self._run_client("release-manifest")
        expected = (ROOT / "apps" / "mem_service" / "release-manifest.txt").read_text()
        self.assertEqual(manifest.returncode, 0, manifest.stderr + manifest.stdout)
        self.assertEqual(manifest.stdout, expected)

    def test_version_cli_reports_release_contract(self):
        version = self._run_client("version")
        self.assertEqual(version.returncode, 0, version.stderr + version.stdout)
        self.assertIn("service_name=linqu_mem_service", version.stdout)
        self.assertIn("service_version=0.1.0", version.stdout)
        self.assertIn("version_contract=text-kv", version.stdout)
        self.assertIn("wire_version=1", version.stdout)
        self.assertIn("wire_schema_manifest_checksum=0xf4cf34c6", version.stdout)
        self.assertIn("api_abi_policy_checksum=0x5d95ae02", version.stdout)
        self.assertIn("package_manifest_len=9126", version.stdout)
        self.assertIn("package_manifest_checksum=0x28945f1f", version.stdout)
        self.assertIn("release_manifest_command=release-manifest", version.stdout)
        self.assertIn("package_manifest_command=package-manifest", version.stdout)
        self.assertIn("config_security_gate=config-fixtures", version.stdout)
        self.assertIn("version_gate=version-fixtures", version.stdout)

        fixtures = self._run_client("version-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("service_version=0.1.0", fixtures.stdout)
        self.assertIn("package_manifest_len=9126", fixtures.stdout)
        self.assertIn("package_manifest_checksum=0x28945f1f", fixtures.stdout)

    def test_release_readiness_cli_reports_external_certification_blockers(self):
        readiness = self._run_client("release-readiness")
        self.assertEqual(readiness.returncode, 0, readiness.stderr + readiness.stdout)
        self.assertIn("mem_service_release_readiness_version=1", readiness.stdout)
        self.assertIn("readiness_contract=text-kv", readiness.stdout)
        self.assertIn("package_manifest_len=9126", readiness.stdout)
        self.assertIn("package_manifest_checksum=0x28945f1f", readiness.stdout)
        self.assertIn(
            "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight",
            readiness.stdout,
        )
        self.assertIn("installed_sdk_contract=machine-discoverable", readiness.stdout)
        self.assertIn("serving_pretraining_runtime=certified", readiness.stdout)
        self.assertIn(
            "ops_certification_status=not-certified-until-external-evidence",
            readiness.stdout,
        )
        self.assertIn(
            "ops_certification_ci=scripts/run_mem_service_linux_ops_ci.sh",
            readiness.stdout,
        )
        self.assertIn(
            "ops_certification_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight",
            readiness.stdout,
        )
        self.assertIn(
            "ops_certification_evidence_verify=ops-certification-verify --evidence-file",
            readiness.stdout,
        )
        self.assertIn(
            "remote_transport_status=not-certified-until-cross-host-evidence",
            readiness.stdout,
        )
        self.assertIn(
            "remote_transport_ci=scripts/run_mem_service_remote_transport_ci.sh",
            readiness.stdout,
        )
        self.assertIn(
            "remote_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight",
            readiness.stdout,
        )
        self.assertIn(
            "remote_transport_evidence_verify=remote-transport-verify --evidence-file",
            readiness.stdout,
        )
        self.assertIn(
            "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh",
            readiness.stdout,
        )
        self.assertIn(
            "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight",
            readiness.stdout,
        )
        self.assertIn(
            "release_certification_verify=scripts/verify_mem_service_release_certification.sh --ops-bundle-file --remote-transport-bundle-file",
            readiness.stdout,
        )
        self.assertIn(
            "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file",
            readiness.stdout,
        )
        self.assertIn(
            "release_readiness_evidence_verify=release-readiness --ops-evidence-file --remote-transport-evidence-file",
            readiness.stdout,
        )
        self.assertIn("overall_status=not-certified", readiness.stdout)
        self.assertIn(
            "blocking_external_evidence=linux-ops-certification-bundle,remote-transport-certification-bundle",
            readiness.stdout,
        )

        fixtures = self._run_client("release-readiness-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("default_overall_status=not-certified", fixtures.stdout)
        self.assertIn("certified_overall_status=certified", fixtures.stdout)
        self.assertIn("evidence_positive=2", fixtures.stdout)
        self.assertIn("fail_closed=1", fixtures.stdout)
        self.assertIn("blocking_external_evidence=2", fixtures.stdout)

    def test_release_readiness_cli_certifies_verified_evidence_files(self):
        ops_evidence = (
            "mem_service_ops_certification_evidence_version=1\n"
            "service_name=linqu_mem_service\n"
            "certification_scope=real-linux-operations\n"
            "evidence_os=linux\n"
            "evidence_init=systemd\n"
            "ops_certification_policy_checksum=0xe77c644b\n"
            "package_manifest_checksum=0x28945f1f\n"
            "linux_systemd_service_smoke=pass\n"
            "linux_systemd_host_service_smoke=pass\n"
            "prometheus_scrape_smoke=pass\n"
            "prometheus_alertmanager_rule_smoke=pass\n"
            "rpm_package_smoke=pass\n"
            "upgrade_rollback_deployment_smoke=pass\n"
        )
        remote_evidence = (
            "mem_service_remote_transport_evidence_version=1\n"
            "service_name=linqu_mem_service\n"
            "certification_scope=production-network-transport\n"
            "transport_backend=transport-tcp-block-v1\n"
            "transport_protocol=tcp-ipv4\n"
            "transport_topology=cross-host\n"
            "package_manifest_checksum=0x28945f1f\n"
            "source_address_non_loopback=pass\n"
            "payload_block_round_trip=pass\n"
            "payload_checksum_validation=pass\n"
            "payload_corruption_fail_closed=pass\n"
            "producer_consumer_distinct_hosts=pass\n"
            "network_partition_fail_closed=pass\n"
        )
        with tempfile.TemporaryDirectory(dir=_tmp_parent()) as tmpdir:
            ops_path = Path(tmpdir) / "ops.evidence"
            remote_path = Path(tmpdir) / "remote.evidence"
            ops_path.write_text(ops_evidence)
            remote_path.write_text(remote_evidence)

            readiness = self._run_client(
                "release-readiness",
                "--ops-evidence-file",
                str(ops_path),
                "--remote-transport-evidence-file",
                str(remote_path),
            )

        self.assertEqual(readiness.returncode, 0, readiness.stderr + readiness.stdout)
        self.assertIn("ops_certification_status=certified", readiness.stdout)
        self.assertIn("remote_transport_status=certified", readiness.stdout)
        self.assertIn("overall_status=certified", readiness.stdout)
        self.assertIn("blocking_external_evidence=none", readiness.stdout)

    def test_release_certification_verifier_reaches_readiness_gate(self):
        ops_evidence = (
            "mem_service_ops_certification_evidence_version=1\n"
            "service_name=linqu_mem_service\n"
            "certification_scope=real-linux-operations\n"
            "evidence_os=linux\n"
            "evidence_init=systemd\n"
            "ops_certification_policy_checksum=0xe77c644b\n"
            "package_manifest_checksum=0x28945f1f\n"
            "linux_systemd_service_smoke=pass\n"
            "linux_systemd_host_service_smoke=pass\n"
            "prometheus_scrape_smoke=pass\n"
            "prometheus_alertmanager_rule_smoke=pass\n"
            "rpm_package_smoke=pass\n"
            "upgrade_rollback_deployment_smoke=pass\n"
        )
        remote_evidence = (
            "mem_service_remote_transport_evidence_version=1\n"
            "service_name=linqu_mem_service\n"
            "certification_scope=production-network-transport\n"
            "transport_backend=transport-tcp-block-v1\n"
            "transport_protocol=tcp-ipv4\n"
            "transport_topology=cross-host\n"
            "package_manifest_checksum=0x28945f1f\n"
            "source_address_non_loopback=pass\n"
            "payload_block_round_trip=pass\n"
            "payload_checksum_validation=pass\n"
            "payload_corruption_fail_closed=pass\n"
            "producer_consumer_distinct_hosts=pass\n"
            "network_partition_fail_closed=pass\n"
        )
        script = ROOT / "scripts" / "verify_mem_service_release_certification.sh"
        app_dir = ROOT / "apps" / "mem_service"

        with tempfile.TemporaryDirectory(dir=_tmp_parent()) as tmpdir:
            tmp = Path(tmpdir)
            ops_root = tmp / "ops-root"
            remote_root = tmp / "remote-root"
            ops_root.mkdir()
            remote_root.mkdir()

            (ops_root / "ops-certification-bundle.manifest").write_text(
                "bundle_schema=linqu-mem-service-ops-certification-bundle-v1\n"
                "bundle_gate=linux-ops-certification-bundle\n"
                "evidence_verify_gate=linux-ops-evidence-verify\n"
                "evidence=ops-certification-linux-ci.evidence\n"
                "upgrade_rollback_marker=ops-certification-upgrade-rollback.marker\n"
                "release_manifest=release-manifest.txt\n"
                "package_manifest=package-manifest.txt\n"
                "ops_certification_policy=ops-certification-policy.txt\n"
                "rpm=linqu-mem-service-0.1.0-1.aarch64.rpm\n"
            )
            (ops_root / "ops-certification-linux-ci.evidence").write_text(ops_evidence)
            (ops_root / "ops-certification-upgrade-rollback.marker").write_text(
                "upgrade_rollback_deployment_smoke=pass\n"
            )
            shutil.copy(app_dir / "release-manifest.txt", ops_root / "release-manifest.txt")
            shutil.copy(app_dir / "package-manifest.txt", ops_root / "package-manifest.txt")
            shutil.copy(
                app_dir / "ops-certification-policy.txt",
                ops_root / "ops-certification-policy.txt",
            )

            (remote_root / "remote-transport-bundle.manifest").write_text(
                "bundle_schema=linqu-mem-service-remote-transport-bundle-v1\n"
                "bundle_gate=remote-transport-certification-bundle\n"
                "evidence_verify_gate=remote-transport-evidence-verify\n"
                "evidence=remote-transport.evidence\n"
                "release_manifest=release-manifest.txt\n"
                "package_manifest=package-manifest.txt\n"
            )
            (remote_root / "remote-transport.evidence").write_text(remote_evidence)
            shutil.copy(
                app_dir / "release-manifest.txt",
                remote_root / "release-manifest.txt",
            )
            shutil.copy(
                app_dir / "package-manifest.txt",
                remote_root / "package-manifest.txt",
            )

            ops_bundle = tmp / "ops.tar"
            remote_bundle = tmp / "remote.tar"
            for bundle_path, root in ((ops_bundle, ops_root), (remote_bundle, remote_root)):
                with tarfile.open(bundle_path, "w") as bundle:
                    for path in root.iterdir():
                        bundle.add(path, arcname=f"./{path.name}")

            result = subprocess.run(
                [
                    str(script),
                    "--ops-bundle-file",
                    str(ops_bundle),
                    "--remote-transport-bundle-file",
                    str(remote_bundle),
                    "--app-dir",
                    str(app_dir),
                    "--work-dir",
                    str(tmp / "verify"),
                ],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )

        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("[mem-service-release-certification] PASS", result.stdout)
        self.assertIn("readiness=certified", result.stdout)

    def test_package_manifest_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("package-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("package_format=installed-layout-v1", fixtures.stdout)
        self.assertIn("manifest_len=9126", fixtures.stdout)
        self.assertIn("manifest_checksum=0x28945f1f", fixtures.stdout)
        self.assertIn("installed_files=46", fixtures.stdout)
        self.assertIn("required_gates=34", fixtures.stdout)

        manifest = self._run_client("package-manifest")
        self.assertEqual(manifest.returncode, 0, manifest.stderr + manifest.stdout)
        self.assertEqual(manifest.stdout, PACKAGE_MANIFEST.read_text())

    def test_runtime_quota_fixtures_cover_record_and_payload_limits(self):
        fixtures = self._run_client("runtime-quota-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("runtime_quota=max-records+max-payload-bytes", fixtures.stdout)
        self.assertIn("max_records=1", fixtures.stdout)
        self.assertIn("max_payload_bytes=24", fixtures.stdout)
        self.assertIn("capacity_exceeded=1", fixtures.stdout)

    def test_retention_fixtures_cover_durable_audit_gc(self):
        fixtures = self._run_client("retention-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("retention_policy=audit-log-limit", fixtures.stdout)
        self.assertIn("max_audit_events=2", fixtures.stdout)
        self.assertIn("retained_events=2", fixtures.stdout)
        self.assertIn("first_sequence=3", fixtures.stdout)
        self.assertIn("record_count=4", fixtures.stdout)
        self.assertIn("durable_reload=1", fixtures.stdout)
        self.assertIn("journal_gc=1", fixtures.stdout)

    def test_checkpoint_retention_fixtures_cover_training_checkpoint_gc(self):
        fixtures = self._run_client("checkpoint-retention-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("checkpoint_retention=latest", fixtures.stdout)
        self.assertIn("max_checkpoint_records=2", fixtures.stdout)
        self.assertIn("retained_checkpoints=2", fixtures.stdout)
        self.assertIn("record_count=3", fixtures.stdout)
        self.assertIn("non_checkpoint_retained=1", fixtures.stdout)
        self.assertIn("durable_reload=1", fixtures.stdout)
        self.assertIn("idempotency_gc=1", fixtures.stdout)
        self.assertIn("journal_gc=1", fixtures.stdout)

    def test_payload_gc_fixtures_cover_orphan_checkpoint_blocks(self):
        fixtures = self._run_client("payload-gc-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("payload_gc=checkpoint-retention-orphan-blocks", fixtures.stdout)
        self.assertIn("payload_blocks_removed=1", fixtures.stdout)
        self.assertIn("shared_block_retained=1", fixtures.stdout)
        self.assertIn("retained_payload_blocks=2", fixtures.stdout)
        self.assertIn("record_count=2", fixtures.stdout)
        self.assertIn("durable_reload=1", fixtures.stdout)
        self.assertIn("journal_gc=1", fixtures.stdout)

    def test_record_retention_fixtures_cover_global_kind_and_tenant_record_gc(self):
        fixtures = self._run_client("record-retention-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("record_retention=latest", fixtures.stdout)
        self.assertIn("max_retained_records=3", fixtures.stdout)
        self.assertIn("record_count=3", fixtures.stdout)
        self.assertIn("pruned_records=2", fixtures.stdout)
        self.assertIn("payload_blocks_removed=1", fixtures.stdout)
        self.assertIn("shared_block_retained=1", fixtures.stdout)
        self.assertIn("idempotency_gc=1", fixtures.stdout)
        self.assertIn("durable_reload=1", fixtures.stdout)
        self.assertIn("journal_gc=1", fixtures.stdout)
        self.assertIn(
            "record_retention=kind:training-artifact:latest",
            fixtures.stdout,
        )
        self.assertIn("retained_training_artifacts=2", fixtures.stdout)
        self.assertIn("non_matching_object_retained=1", fixtures.stdout)
        self.assertIn("record_retention=tenant:7:latest", fixtures.stdout)
        self.assertIn("retained_tenant_records=2", fixtures.stdout)
        self.assertIn("non_matching_tenant_retained=2", fixtures.stdout)
        self.assertIn("record_retention=kind:training-artifact:ttl-ms", fixtures.stdout)
        self.assertIn("max_retained_record_age_ms=60000", fixtures.stdout)
        self.assertIn("pruned_expired_records=1", fixtures.stdout)
        self.assertIn("fresh_record_retained=1", fixtures.stdout)

    def test_encryption_policy_is_explicit_none_fail_closed(self):
        policy = self._run_client("encryption-policy")
        self.assertEqual(policy.returncode, 0, policy.stderr + policy.stdout)
        self.assertIn("mem_service_encryption_policy_version=1", policy.stdout)
        self.assertIn("encryption_at_rest=not-certified", policy.stdout)
        self.assertIn("supported_config_encryption=none", policy.stdout)
        self.assertIn("unsupported_encryption_admission=fail-closed", policy.stdout)
        self.assertIn("policy_gate=encryption-fixtures", policy.stdout)

        fixtures = self._run_client("encryption-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("encryption_policy=explicit-none-only", fixtures.stdout)
        self.assertIn("encryption_at_rest=not-certified", fixtures.stdout)
        self.assertIn("unsupported_encryption_admission=fail-closed", fixtures.stdout)
        self.assertIn("fail_closed_invalid=1", fixtures.stdout)

    def test_config_fixtures_enforce_local_auth_boundary(self):
        fixtures = self._run_client("config-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn(
            "service_auth_boundary=unix-socket-local-only",
            fixtures.stdout,
        )
        self.assertIn("metrics_auth_boundary=loopback-only", fixtures.stdout)
        self.assertIn(
            "quota_contract=max-records+max-payload-bytes",
            fixtures.stdout,
        )
        self.assertIn("max_records=1024", fixtures.stdout)
        self.assertIn("max_payload_bytes=4096", fixtures.stdout)
        self.assertIn("retention=manual", fixtures.stdout)
        self.assertIn("encryption=none", fixtures.stdout)
        self.assertIn("encryption_admission=explicit-none-only", fixtures.stdout)
        self.assertIn("checkpoint_retention=manual", fixtures.stdout)
        self.assertIn("record_retention=kind:training-artifact:latest:2", fixtures.stdout)
        self.assertIn("tenant_record_retention=tenant:7:latest:2", fixtures.stdout)
        self.assertIn("ttl_record_retention=kind:training-artifact:ttl-ms:60000", fixtures.stdout)
        self.assertIn("fail_closed_invalid=6", fixtures.stdout)

    def test_daemon_config_runtime_quota_admission(self):
        quota_config = self.root / "quota.conf"
        quota_config.write_text(
            f"listen=unix:{self.socket}\n"
            f"store={self.store}\n"
            "backend=snapshot+journal\n"
            "max_records=1\n"
            "max_payload_bytes=4096\n"
            "retention=manual\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
        )
        proc = self._start_server(config_path=quota_config)
        try:
            first = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "quota-object-1",
                "--version",
                "1",
                "--checksum",
                "11",
                "--backing-len",
                "8",
            )
            self.assertEqual(first.returncode, 0, first.stderr + first.stdout)
            self.assertIn("status=ok", first.stdout)
            second = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "quota-object-2",
                "--version",
                "2",
                "--checksum",
                "22",
                "--backing-len",
                "8",
            )
            self.assertNotEqual(second.returncode, 0, second.stderr + second.stdout)
            self.assertIn("status=capacity_exceeded", second.stdout)
            self.assertIn("quota=max_records", second.stdout)
            status = self._run_client("status", "--connect", f"unix:{self.socket}")
            self.assertEqual(status.returncode, 0, status.stderr + status.stdout)
            self.assertIn("record_count=1", status.stdout)
            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            self.assertIn("capacity_exceeded_count=1", metrics.stdout)
        finally:
            stdout, _, _ = self._stop_server_and_collect(proc)
        self.assertIn("max_records=1", stdout)
        self.assertIn("max_payload_bytes=4096", stdout)

        payload_config = self.root / "payload-quota.conf"
        payload_config.write_text(
            f"listen=unix:{self.socket}\n"
            f"store={self.root / 'payload.store'}\n"
            "backend=snapshot+journal\n"
            "max_records=1024\n"
            "max_payload_bytes=24\n"
            "retention=manual\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
        )
        payload_proc = self._start_server(config_path=payload_config)
        try:
            rejected = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "payload-quota-object",
                "--version",
                "1",
                "--checksum",
                "33",
                "--backing-len",
                "8",
            )
            self.assertNotEqual(rejected.returncode, 0, rejected.stderr + rejected.stdout)
            self.assertIn("status=capacity_exceeded", rejected.stdout)
            self.assertIn("quota=max_payload_bytes", rejected.stdout)
            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            self.assertIn("capacity_exceeded_count=1", metrics.stdout)
        finally:
            stdout, _, _ = self._stop_server_and_collect(payload_proc)
        self.assertIn("max_payload_bytes=24", stdout)

    def test_daemon_config_retention_limits_audit_log(self):
        retention_config = self.root / "retention.conf"
        retention_config.write_text(
            f"listen=unix:{self.socket}\n"
            f"store={self.store}\n"
            "backend=snapshot+journal\n"
            "retention=audit-log:2\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
        )
        proc = self._start_server(config_path=retention_config)
        try:
            for index in range(4):
                put = self._run_client(
                    "put-object",
                    "--connect",
                    f"unix:{self.socket}",
                    "--key",
                    f"retention-config-object-{index + 1}",
                    "--version",
                    str(index + 1),
                    "--checksum",
                    str(100 + index),
                    "--backing-len",
                    "8",
                )
                self.assertEqual(put.returncode, 0, put.stderr + put.stdout)
            audit = self._run_client(
                "audit-log",
                "--connect",
                f"unix:{self.socket}",
                "--max-events",
                "8",
            )
            self.assertEqual(audit.returncode, 0, audit.stderr + audit.stdout)
            self.assertIn("retained_events=2", audit.stdout)
            self.assertIn("first_sequence=3", audit.stdout)
            self.assertNotIn("retention-config-object-1", audit.stdout)
            self.assertNotIn("retention-config-object-2", audit.stdout)
            self.assertIn("retention-config-object-3", audit.stdout)
            self.assertIn("retention-config-object-4", audit.stdout)
        finally:
            stdout, _, _ = self._stop_server_and_collect(proc)
        self.assertIn("max_audit_events=2", stdout)

    def test_daemon_config_checkpoint_retention_limits_checkpoint_records(self):
        retention_config = self.root / "checkpoint_retention.conf"
        retention_config.write_text(
            f"listen=unix:{self.socket}\n"
            f"store={self.store}\n"
            "backend=snapshot+journal\n"
            "checkpoint_retention=latest:2\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
        )
        proc = self._start_server(config_path=retention_config)
        try:
            for index in range(4):
                register = self._run_client(
                    "register-training-artifact",
                    "--connect",
                    f"unix:{self.socket}",
                    "--key",
                    f"training/run-retention/checkpoint-{index + 1}",
                    "--session-id",
                    "run-retention",
                    "--model-key",
                    "model-retention",
                    "--artifact-kind",
                    "checkpoint",
                    "--artifact-id",
                    f"checkpoint-{index + 1}",
                    "--version",
                    str(index + 1),
                    "--checksum",
                    str(500 + index),
                    "--backing-len",
                    "8",
                )
                self.assertEqual(register.returncode, 0, register.stderr + register.stdout)
            gradient = self._run_client(
                "register-training-artifact",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "training/run-retention/gradient-1",
                "--session-id",
                "run-retention",
                "--model-key",
                "model-retention",
                "--artifact-kind",
                "gradient",
                "--artifact-id",
                "gradient-1",
                "--version",
                "1",
                "--checksum",
                "900",
                "--backing-len",
                "8",
            )
            self.assertEqual(gradient.returncode, 0, gradient.stderr + gradient.stdout)

            records = self._run_client("list-records", "--connect", f"unix:{self.socket}")
            self.assertEqual(records.returncode, 0, records.stderr + records.stdout)
            self.assertNotIn("key=training/run-retention/checkpoint-1", records.stdout)
            self.assertNotIn("key=training/run-retention/checkpoint-2", records.stdout)
            self.assertIn("key=training/run-retention/checkpoint-3", records.stdout)
            self.assertIn("key=training/run-retention/checkpoint-4", records.stdout)
            self.assertIn("key=training/run-retention/gradient-1", records.stdout)
        finally:
            stdout, _, _ = self._stop_server_and_collect(proc)
        self.assertIn("max_checkpoint_records=2", stdout)
        store_text = self.store.read_text()
        store_records = store_text.split("audit_begin", 1)[0]
        self.assertIn("record_count=3", store_records)
        self.assertNotIn("key=training/run-retention/checkpoint-1", store_records)
        self.assertNotIn("key=training/run-retention/checkpoint-2", store_records)
        self.assertIn("key=training/run-retention/checkpoint-3", store_records)
        self.assertIn("key=training/run-retention/checkpoint-4", store_records)
        self.assertIn("key=training/run-retention/gradient-1", store_records)

    def test_restore_policy_fixtures_fail_closed_until_commit(self):
        fixtures = self._run_client("restore-policy-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("restore_policy=transactional-staged-restore", fixtures.stdout)
        self.assertIn(
            "restore_scope=full-snapshot,paged-snapshot",
            fixtures.stdout,
        )
        self.assertIn("full_restore=ok", fixtures.stdout)
        self.assertIn("paged_restore=ok", fixtures.stdout)
        self.assertIn(
            "fail_closed_cases=bad-magic,out-of-order-page,record-count-mismatch,"
            "cancelled-stage-commit",
            fixtures.stdout,
        )
        self.assertIn("live_state=unchanged-until-commit", fixtures.stdout)
        self.assertIn("invalid_session=2", fixtures.stdout)
        self.assertIn("version_conflict=2", fixtures.stdout)
        self.assertIn("fail_closed=4", fixtures.stdout)

    def test_ops_certification_policy_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("ops-certification-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("policy_len=1118", fixtures.stdout)
        self.assertIn("policy_checksum=0xe77c644b", fixtures.stdout)
        self.assertIn("certification_status=not-certified", fixtures.stdout)
        self.assertIn(
            "admission_rule=fail-closed-until-external-evidence", fixtures.stdout
        )

        policy = self._run_client("ops-certification-policy")
        self.assertEqual(policy.returncode, 0, policy.stderr + policy.stdout)
        self.assertEqual(policy.stdout, OPS_CERTIFICATION_POLICY.read_text())

        evidence = (
            "mem_service_ops_certification_evidence_version=1\n"
            "service_name=linqu_mem_service\n"
            "certification_scope=real-linux-operations\n"
            "evidence_os=linux\n"
            "evidence_init=systemd\n"
            "ops_certification_policy_checksum=0xe77c644b\n"
            "package_manifest_checksum=0x28945f1f\n"
            "linux_systemd_service_smoke=pass\n"
            "linux_systemd_host_service_smoke=pass\n"
            "prometheus_scrape_smoke=pass\n"
            "prometheus_alertmanager_rule_smoke=pass\n"
            "rpm_package_smoke=pass\n"
            "upgrade_rollback_deployment_smoke=pass\n"
        )
        bad_evidence = evidence.replace("rpm_package_smoke=pass", "rpm_package_smoke=fail")
        with tempfile.TemporaryDirectory(prefix="msvc_ops_evidence_", dir=str(_tmp_parent())) as tmp:
            evidence_path = Path(tmp) / "ops.evidence"
            bad_evidence_path = Path(tmp) / "ops.bad.evidence"
            evidence_path.write_text(evidence)
            bad_evidence_path.write_text(bad_evidence)

            verified = self._run_client(
                "ops-certification-verify", "--evidence-file", str(evidence_path)
            )
            self.assertEqual(verified.returncode, 0, verified.stderr + verified.stdout)
            self.assertIn("certification_status=certified", verified.stdout)
            self.assertIn("external_gates=6", verified.stdout)

            rejected = self._run_client(
                "ops-certification-verify", "--evidence-file", str(bad_evidence_path)
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("fail-closed", rejected.stderr)
            self.assertIn("rpm_package_smoke", rejected.stderr)

        evidence_fixtures = self._run_client("ops-certification-evidence-fixtures")
        self.assertEqual(
            evidence_fixtures.returncode,
            0,
            evidence_fixtures.stderr + evidence_fixtures.stdout,
        )
        self.assertIn("evidence_schema=ops-certification-evidence-v1",
                      evidence_fixtures.stdout)
        self.assertIn("fail_closed=2", evidence_fixtures.stdout)

        generated = self._run_client("ops-certification-generate-evidence")
        self.assertEqual(generated.returncode, 0, generated.stderr + generated.stdout)
        self.assertIn(
            "evidence_generator=ops-certification-generate-evidence",
            generated.stdout,
        )
        self.assertIn("ops_certification_policy_checksum=0xe77c644b", generated.stdout)
        self.assertIn("package_manifest_checksum=0x28945f1f", generated.stdout)
        self.assertIn("rpm_package_smoke=fail", generated.stdout)

        with tempfile.TemporaryDirectory(prefix="msvc_ops_probe_", dir=str(_tmp_parent())) as tmp:
            generated_path = Path(tmp) / "ops.generated.evidence"
            generated_path.write_text(generated.stdout)
            generated_rejected = self._run_client(
                "ops-certification-verify", "--evidence-file", str(generated_path)
            )
            self.assertNotEqual(generated_rejected.returncode, 0)
            self.assertIn("fail-closed", generated_rejected.stderr)

            linux_ci_path = Path(tmp) / "ops.linux-ci.evidence"
            linux_ci = self._run_client(
                "ops-certification-linux-ci-smoke",
                "--evidence-file",
                str(linux_ci_path),
            )
            self.assertNotEqual(linux_ci.returncode, 0)
            self.assertTrue(linux_ci_path.exists())
            self.assertIn("fail-closed", linux_ci.stderr)
            self.assertIn(
                "evidence_generator=ops-certification-generate-evidence",
                linux_ci_path.read_text(),
            )

    def test_api_abi_policy_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("api-abi-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("policy_len=856", fixtures.stdout)
        self.assertIn("policy_checksum=0x5d95ae02", fixtures.stdout)
        self.assertIn("client_record_abi_size=744", fixtures.stdout)

        policy = self._run_client("api-abi-policy")
        self.assertEqual(policy.returncode, 0, policy.stderr + policy.stdout)
        self.assertEqual(policy.stdout, API_ABI_POLICY.read_text())

    def test_admin_output_schema_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("admin-output-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("schema_len=6624", fixtures.stdout)
        self.assertIn("schema_checksum=0x7021f4cf", fixtures.stdout)
        self.assertIn("prometheus_prefix=lingqu_mem_service_", fixtures.stdout)

        schema = self._run_client("admin-output-schema")
        self.assertEqual(schema.returncode, 0, schema.stderr + schema.stdout)
        self.assertEqual(schema.stdout, ADMIN_OUTPUT_SCHEMA.read_text())

    def test_upgrade_rollback_policy_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("upgrade-rollback-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("policy_len=2019", fixtures.stdout)
        self.assertIn("policy_checksum=0xf7943816", fixtures.stdout)
        self.assertIn("required_gates=19", fixtures.stdout)
        self.assertIn("upgrade_policy=current-version-only", fixtures.stdout)
        self.assertIn("rollback_policy=current-version-only", fixtures.stdout)

        policy = self._run_client("upgrade-rollback-policy")
        self.assertEqual(policy.returncode, 0, policy.stderr + policy.stdout)
        self.assertEqual(policy.stdout, UPGRADE_ROLLBACK_POLICY.read_text())
        self.assertIn(
            "same_version_runtime_gate=upgrade-rollback-runtime-fixtures",
            policy.stdout,
        )

        runtime = self._run_client("upgrade-rollback-runtime-fixtures")
        self.assertEqual(runtime.returncode, 0, runtime.stderr + runtime.stdout)
        self.assertIn("status=ok", runtime.stdout)
        self.assertIn("same_version_restart=store-snapshot+journal", runtime.stdout)
        self.assertIn(
            "same_version_upgrade=export-snapshot+restore-snapshot",
            runtime.stdout,
        )
        self.assertIn("same_version_rollback=baseline-snapshot-restore", runtime.stdout)
        self.assertIn("pretraining_commits=1", runtime.stdout)
        self.assertIn(
            "release_admission=reject-unknown-release-generation",
            runtime.stdout,
        )

    def test_alert_rules_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("alert-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("format=prometheus-rules-yaml", fixtures.stdout)
        self.assertIn("rules=6", fixtures.stdout)
        self.assertIn("rules_len=2096", fixtures.stdout)
        self.assertIn("rules_checksum=0x05a9245c", fixtures.stdout)

        rules = self._run_client("alert-rules")
        self.assertEqual(rules.returncode, 0, rules.stderr + rules.stdout)
        self.assertEqual(rules.stdout, ALERT_RULES.read_text())

        integration = self._run_client("alert-integration-fixtures")
        self.assertEqual(integration.returncode, 0, integration.stderr + integration.stdout)
        self.assertIn("status=ok", integration.stdout)
        self.assertIn("collector=prometheus-text-http", integration.stdout)
        self.assertIn("alert_rules=6", integration.stdout)
        self.assertIn("referenced_metrics=5", integration.stdout)

    def test_durable_catalog_fixtures_cli_validates_storage_root_layout(self):
        fixtures = self._run_client("durable-catalog-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("layout=storage-root-v1", fixtures.stdout)
        self.assertIn("payload_block_backend=sealed-local-block-v1", fixtures.stdout)
        self.assertIn("sealed-chunked-block-v1", fixtures.stdout)
        self.assertIn("catalog_schema_version=1", fixtures.stdout)
        self.assertIn(
            "migration_policy=accept-current-reject-future", fixtures.stdout
        )

    def test_chunked_block_fixtures_cli_writes_validates_and_quarantines(self):
        fixtures = self._run_client("chunked-block-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("payload_block_backend=sealed-chunked-block-v1", fixtures.stdout)
        self.assertIn("chunk_size=1024", fixtures.stdout)
        self.assertIn("chunks=3", fixtures.stdout)
        self.assertIn("total_len=2500", fixtures.stdout)
        # The fixture corrupts a byte inside a chunk's read window and proves the
        # reassembled checksum no longer matches -> fail-closed quarantine.
        self.assertIn("integrity=fail-closed-quarantine", fixtures.stdout)

    def test_transport_block_fixtures_cli_writes_validates_and_quarantines(self):
        fixtures = self._run_client("transport-block-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("payload_block_backend=transport-loopback-block-v1", fixtures.stdout)
        self.assertIn("transport=file-copy-v1", fixtures.stdout)
        self.assertIn("total_len=1537", fixtures.stdout)
        self.assertIn("integrity=fail-closed-quarantine", fixtures.stdout)
        self.assertIn("network_transport=not-certified", fixtures.stdout)

    def test_network_transport_block_fixtures_cli_fetches_tcp_payload(self):
        fixtures = self._run_client("network-transport-block-fixtures")
        if (
            fixtures.returncode != 0
            and "tcp source setup failed" in fixtures.stderr + fixtures.stdout
        ):
            raise unittest.SkipTest("sandbox forbids TCP payload source bind")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("payload_block_backend=transport-tcp-block-v1", fixtures.stdout)
        self.assertIn("transport=tcp-loopback-v1", fixtures.stdout)
        self.assertIn("total_len=2049", fixtures.stdout)
        self.assertIn("integrity=fail-closed-quarantine", fixtures.stdout)
        self.assertIn("network_transport=tcp-loopback-certified", fixtures.stdout)

    def test_remote_block_backend_policy_fixtures_cli_marks_loopback_certified(self):
        fixtures = self._run_client("remote-block-backend-policy-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn(
            "remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1",
            fixtures.stdout,
        )
        self.assertIn(
            "remote_backend_admission=loopback-and-tcp-loopback-certified",
            fixtures.stdout,
        )
        self.assertIn(
            "remote_payload_block_data_gate=transport-block-fixtures",
            fixtures.stdout,
        )
        self.assertIn(
            "remote_payload_network_transport=tcp-loopback-certified",
            fixtures.stdout,
        )
        self.assertIn(
            "remote_payload_network_transport_gate=network-transport-block-fixtures",
            fixtures.stdout,
        )
        self.assertIn(
            "current_payload_block_backends=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1",
            fixtures.stdout,
        )

    def test_remote_transport_evidence_fixtures_and_verify_fail_closed(self):
        fixtures = self._run_client("remote-transport-evidence-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("evidence_schema=remote-transport-evidence-v1", fixtures.stdout)
        self.assertIn("fail_closed=3", fixtures.stdout)
        self.assertIn("external_gates=6", fixtures.stdout)
        self.assertIn(
            "certification_status=not-certified-until-cross-host-evidence",
            fixtures.stdout,
        )

        evidence = (
            "mem_service_remote_transport_evidence_version=1\n"
            "service_name=linqu_mem_service\n"
            "certification_scope=production-network-transport\n"
            "transport_backend=transport-tcp-block-v1\n"
            "transport_protocol=tcp-ipv4\n"
            "transport_topology=cross-host\n"
            "package_manifest_checksum=0x28945f1f\n"
            "source_address_non_loopback=pass\n"
            "payload_block_round_trip=pass\n"
            "payload_checksum_validation=pass\n"
            "payload_corruption_fail_closed=pass\n"
            "producer_consumer_distinct_hosts=pass\n"
            "network_partition_fail_closed=pass\n"
        )
        bad_evidence = evidence.replace(
            "transport_topology=cross-host", "transport_topology=loopback"
        )
        with tempfile.TemporaryDirectory(prefix="msvc_remote_transport_", dir=str(_tmp_parent())) as tmp:
            evidence_path = Path(tmp) / "remote_transport.evidence"
            bad_evidence_path = Path(tmp) / "remote_transport.bad.evidence"
            evidence_path.write_text(evidence)
            bad_evidence_path.write_text(bad_evidence)

            verified = self._run_client(
                "remote-transport-verify", "--evidence-file", str(evidence_path)
            )
            self.assertEqual(verified.returncode, 0, verified.stderr + verified.stdout)
            self.assertIn("certification_status=certified", verified.stdout)
            self.assertIn("external_gates=6", verified.stdout)

            rejected = self._run_client(
                "remote-transport-verify", "--evidence-file", str(bad_evidence_path)
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("fail-closed", rejected.stderr)
            self.assertIn("bad-evidence-identity", rejected.stderr)

    def test_remote_transport_generate_evidence_fetches_payload_but_rejects_loopback(self):
        payload = b"remote transport generated evidence payload"
        ready = threading.Event()
        port_holder = {}

        def serve_once():
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind(("127.0.0.1", 0))
                server.listen(1)
                port_holder["port"] = server.getsockname()[1]
                ready.set()
                conn, _ = server.accept()
                with conn:
                    conn.sendall(payload)

        thread = threading.Thread(target=serve_once, daemon=True)
        thread.start()
        self.assertTrue(ready.wait(timeout=5.0))
        with tempfile.TemporaryDirectory(prefix="msvc_remote_transport_gen_", dir=str(_tmp_parent())) as tmp:
            tmp_path = Path(tmp)
            marker = tmp_path / "partition.marker"
            evidence_path = tmp_path / "remote_transport.evidence"
            storage_root = tmp_path / "storage"
            marker.write_text("network_partition_fail_closed=pass\n")

            generated = self._run_client(
                "remote-transport-generate-evidence",
                "--source",
                f"tcp:127.0.0.1:{port_holder['port']}",
                "--producer-host",
                "producer-a",
                "--consumer-host",
                "consumer-b",
                "--network-partition-marker",
                str(marker),
                "--evidence-file",
                str(evidence_path),
                "--storage-root",
                str(storage_root),
            )
            thread.join(timeout=5.0)
            self.assertNotEqual(generated.returncode, 0)
            self.assertIn("fail-closed", generated.stderr)
            self.assertIn("source_address_non_loopback", generated.stderr)
            evidence = evidence_path.read_text()
            self.assertIn("evidence_generator=remote-transport-generate-evidence", evidence)
            self.assertIn("source_address_non_loopback=fail", evidence)
            self.assertIn("payload_block_round_trip=pass", evidence)
            self.assertIn("payload_checksum_validation=pass", evidence)
            self.assertIn("payload_corruption_fail_closed=pass", evidence)
            self.assertIn("producer_consumer_distinct_hosts=pass", evidence)
            self.assertIn("network_partition_fail_closed=pass", evidence)

    def test_deployment_fixtures_cli_validates_service_and_metrics_scrape_contract(self):
        fixtures = self._run_client("deployment-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("deployment_smoke_version=1", fixtures.stdout)
        self.assertIn("service_manager=systemd-like", fixtures.stdout)
        self.assertIn("host_service_manager=systemd-like", fixtures.stdout)
        self.assertIn("metrics_scrape_path=/metrics", fixtures.stdout)
        self.assertIn("metrics_http_content_type=prometheus-text", fixtures.stdout)

    def test_collector_fixtures_cli_validates_prometheus_collector_contract(self):
        fixtures = self._run_client("collector-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("collector=prometheus-text-http", fixtures.stdout)
        self.assertIn("metrics=5", fixtures.stdout)

    def test_daemon_http_metrics_listener_serves_prometheus_scrape(self):
        metrics_port = self._free_tcp_port()
        proc = self._start_server(metrics_port=metrics_port)
        try:
            put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "http-metrics-object",
                "--version",
                "1",
                "--checksum",
                "7432",
            )
            self.assertEqual(put.returncode, 0, put.stderr + put.stdout)

            response = self._http_metrics_request(metrics_port)
            self.assertIn("HTTP/1.1 200 OK\r\n", response)
            self.assertIn("Content-Type: text/plain; version=0.0.4\r\n", response)
            self.assertIn("Cache-Control: no-store\r\n", response)
            self.assertIn("# TYPE lingqu_mem_service_request_count counter\n", response)
            self.assertIn("lingqu_mem_service_put_object_count 1\n", response)
            self.assertIn("lingqu_mem_service_request_latency_max_ms", response)

            missing = self._http_metrics_request(metrics_port, path="/bad")
            self.assertIn("HTTP/1.1 404 Not Found\r\n", missing)
            self.assertIn("not_found\n", missing)

            wrong_method = self._http_metrics_request(metrics_port, method="POST")
            self.assertIn("HTTP/1.1 405 Method Not Allowed\r\n", wrong_method)
            self.assertIn("method_not_allowed\n", wrong_method)
        finally:
            self._stop_server(proc)

    def test_service_manager_lifecycle_runs_config_ready_scrape_and_shutdown(self):
        metrics_port = self._free_tcp_port()
        config_path = self.root / "mem_service.conf"
        config_path.write_text(
            f"listen=unix:{self.socket}\n"
            f"store={self.store}\n"
            "backend=snapshot+journal\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
            f"metrics_listen=tcp:127.0.0.1:{metrics_port}\n"
            "adapter_enablement=core\n"
        )
        proc = self._start_server(config_path=config_path)
        stdout = ""
        stderr = ""
        rc = None
        try:
            health = self._run_client("health", "--connect", f"unix:{self.socket}")
            self.assertEqual(health.returncode, 0, health.stderr + health.stdout)
            self.assertIn("status=ok", health.stdout)

            scraped = self._http_metrics_request(metrics_port)
            self.assertIn("HTTP/1.1 200 OK\r\n", scraped)
            self.assertRegex(scraped, r"lingqu_mem_service_health_count [1-9][0-9]*\n")

            stdout, stderr, rc = self._stop_server_and_collect(proc)
            self.assertEqual(rc, 0, stderr + stdout)
            self.assertIn("status=ready", stdout)
            self.assertIn(f"listen=unix:{self.socket}", stdout)
            self.assertIn(f"store={self.store}", stdout)
            self.assertIn(f"metrics_listen=tcp:127.0.0.1:{metrics_port}", stdout)
            self.assertIn("status=stopped", stdout)
            self.assertFalse(self.socket.exists(), "service socket should be removed on shutdown")
        finally:
            if proc.poll() is None:
                self._stop_server(proc)

    def test_storage_root_catalog_derives_store_and_recovers_object(self):
        storage_root = self.root / "durable-root"
        config_path = self.root / "mem_service.catalog.conf"
        store_path = storage_root / "catalog" / "store.snapshot"
        journal_path = storage_root / "catalog" / "store.snapshot.journal"
        manifest_path = storage_root / "catalog" / "manifest.txt"
        config_path.write_text(
            f"listen=unix:{self.socket}\n"
            f"storage_root={storage_root}\n"
            "backend=snapshot+journal\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
            "adapter_enablement=core\n"
        )
        proc = self._start_server(config_path=config_path)
        try:
            put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "catalog-object",
                "--version",
                "11",
                "--checksum",
                "11011",
                "--backing-len",
                "256",
            )
            self.assertEqual(put.returncode, 0, put.stderr + put.stdout)
        finally:
            stdout, stderr, rc = self._stop_server_and_collect(proc)
        self.assertEqual(rc, 0, stderr + stdout)
        self.assertIn(f"store={store_path}", stdout)
        self.assertIn(f"storage_root={storage_root}", stdout)
        self.assertTrue((storage_root / "catalog").is_dir())
        self.assertTrue((storage_root / "blocks").is_dir())
        self.assertTrue((storage_root / "quarantine").is_dir())
        self.assertTrue(manifest_path.exists())
        manifest = manifest_path.read_text()
        self.assertIn("mem_service_durable_catalog_v1", manifest)
        self.assertIn("layout=storage-root-v1", manifest)
        self.assertIn(f"store_path={store_path}", manifest)
        self.assertIn(f"journal_path={journal_path}", manifest)
        self.assertIn("payload_block_backend=sealed-local-block-v1", manifest)
        self.assertIn("corrupt_payload_policy=quarantine-fail-closed", manifest)
        self.assertTrue(store_path.exists())
        self.assertTrue(journal_path.exists())
        self.assertIn("key=catalog-object", store_path.read_text())

        proc = self._start_server(config_path=config_path)
        try:
            recovered = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "catalog-object",
            )
            self.assertEqual(recovered.returncode,
                             0,
                             recovered.stderr + recovered.stdout)
            self.assertIn("status=ok", recovered.stdout)
            self.assertIn("version=11", recovered.stdout)
            self.assertIn("object_payload_checksum=11011", recovered.stdout)
        finally:
            self._stop_server(proc)

    def test_storage_root_payload_inline_block_validates_and_fail_closes(self):
        storage_root = self.root / "payload-root"
        config_path = self.root / "mem_service.payload.conf"
        object_payload = b"sealed-object-payload-v1"
        artifact_payload = b"sealed-training-payload-v1"
        object_checksum = _fnv1a64(object_payload)
        artifact_checksum = _fnv1a64(artifact_payload)
        object_block = storage_root / "blocks" / f"{object_checksum:016x}.block"
        artifact_block = storage_root / "blocks" / f"{artifact_checksum:016x}.block"
        quarantine_dir = storage_root / "quarantine"

        config_path.write_text(
            f"listen=unix:{self.socket}\n"
            f"storage_root={storage_root}\n"
            "backend=snapshot+journal\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
            "adapter_enablement=core\n"
        )
        proc = self._start_server(config_path=config_path)
        try:
            put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-object",
                "--version",
                "12",
                "--backing-len",
                str(len(object_payload)),
                "--checksum",
                str(object_checksum),
                "--payload-inline",
                object_payload.decode("ascii"),
            )
            self.assertEqual(put.returncode, 0, put.stderr + put.stdout)
            self.assertTrue(object_block.exists())
            self.assertEqual(object_block.read_bytes(), object_payload)

            got = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-object",
            )
            self.assertEqual(got.returncode, 0, got.stderr + got.stdout)
            self.assertIn("object_payload_kind=64", got.stdout)
            self.assertIn(f"object_backing_len={len(object_payload)}", got.stdout)
            self.assertIn(f"object_payload_checksum={object_checksum}", got.stdout)

            register = self._run_client(
                "register-training-artifact",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "training/run-c/checkpoint-0001",
                "--session-id",
                "run-c",
                "--model-key",
                "model-c",
                "--artifact-kind",
                "checkpoint",
                "--artifact-id",
                "checkpoint-0001",
                "--version",
                "3",
                "--backing-len",
                str(len(artifact_payload)),
                "--checksum",
                str(artifact_checksum),
                "--payload-inline",
                artifact_payload.decode("ascii"),
            )
            self.assertEqual(register.returncode, 0, register.stderr + register.stdout)
            self.assertTrue(artifact_block.exists())
            self.assertEqual(artifact_block.read_bytes(), artifact_payload)

            query = self._run_client(
                "query-training-artifact",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "training/run-c/checkpoint-0001",
                "--expected-session-id",
                "run-c",
                "--expected-model-key",
                "model-c",
                "--expected-artifact-kind",
                "checkpoint",
                "--expected-artifact-id",
                "checkpoint-0001",
                "--expected-version",
                "3",
                "--expected-checksum",
                str(artifact_checksum),
            )
            self.assertEqual(query.returncode, 0, query.stderr + query.stdout)
            self.assertIn("object_payload_kind=64", query.stdout)

            object_block.write_bytes(b"corrupt-object")
            bad_get = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-object",
            )
            self.assertNotEqual(bad_get.returncode, 0, bad_get.stderr + bad_get.stdout)
            self.assertIn("status=checksum_mismatch", bad_get.stdout)
            self.assertFalse(object_block.exists())
            self.assertTrue(list(quarantine_dir.glob(f"{object_checksum:016x}.bad.*")))

            artifact_block.write_bytes(b"corrupt-artifact")
            bad_query = self._run_client(
                "query-training-artifact",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "training/run-c/checkpoint-0001",
                "--expected-session-id",
                "run-c",
                "--expected-model-key",
                "model-c",
                "--expected-artifact-kind",
                "checkpoint",
                "--expected-artifact-id",
                "checkpoint-0001",
                "--expected-version",
                "3",
                "--expected-checksum",
                str(artifact_checksum),
            )
            self.assertNotEqual(bad_query.returncode, 0, bad_query.stderr + bad_query.stdout)
            self.assertIn("status=checksum_mismatch", bad_query.stdout)
            self.assertFalse(artifact_block.exists())
            self.assertTrue(list(quarantine_dir.glob(f"{artifact_checksum:016x}.bad.*")))
        finally:
            self._stop_server(proc)

    def test_storage_root_payload_file_block_ingests_large_payload(self):
        storage_root = self.root / "payload-file-root"
        config_path = self.root / "mem_service.payload_file.conf"
        object_payload = bytes((i % 251 for i in range(8197)))
        artifact_payload = bytes(((i * 7) % 253 for i in range(6149)))
        object_source = self.root / "large-object-payload.bin"
        artifact_source = self.root / "large-artifact-payload.bin"
        object_checksum = _fnv1a64(object_payload)
        artifact_checksum = _fnv1a64(artifact_payload)
        object_block = storage_root / "blocks" / f"{object_checksum:016x}.block"
        artifact_block = storage_root / "blocks" / f"{artifact_checksum:016x}.block"
        quarantine_dir = storage_root / "quarantine"

        object_source.write_bytes(object_payload)
        artifact_source.write_bytes(artifact_payload)
        config_path.write_text(
            f"listen=unix:{self.socket}\n"
            f"storage_root={storage_root}\n"
            "backend=snapshot+journal\n"
            "auth_mode=none\n"
            "metrics_mode=text-kv\n"
            "adapter_enablement=core\n"
        )
        proc = self._start_server(config_path=config_path)
        try:
            put = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-file-object",
                "--version",
                "31",
                "--backing-len",
                str(len(object_payload)),
                "--checksum",
                str(object_checksum),
                "--payload-file",
                str(object_source),
            )
            self.assertEqual(put.returncode, 0, put.stderr + put.stdout)
            self.assertTrue(object_block.exists())
            self.assertEqual(object_block.read_bytes(), object_payload)

            got = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-file-object",
            )
            self.assertEqual(got.returncode, 0, got.stderr + got.stdout)
            self.assertIn("object_payload_kind=64", got.stdout)
            self.assertIn(f"object_backing_len={len(object_payload)}", got.stdout)
            self.assertIn(f"object_payload_checksum={object_checksum}", got.stdout)

            register = self._run_client(
                "register-training-artifact",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "training/run-file/checkpoint-0002",
                "--session-id",
                "run-file",
                "--model-key",
                "model-file",
                "--artifact-kind",
                "checkpoint",
                "--artifact-id",
                "checkpoint-0002",
                "--version",
                "5",
                "--backing-len",
                str(len(artifact_payload)),
                "--checksum",
                str(artifact_checksum),
                "--payload-file",
                str(artifact_source),
            )
            self.assertEqual(register.returncode, 0, register.stderr + register.stdout)
            self.assertTrue(artifact_block.exists())
            self.assertEqual(artifact_block.read_bytes(), artifact_payload)

            query = self._run_client(
                "query-training-artifact",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "training/run-file/checkpoint-0002",
                "--expected-session-id",
                "run-file",
                "--expected-model-key",
                "model-file",
                "--expected-artifact-kind",
                "checkpoint",
                "--expected-artifact-id",
                "checkpoint-0002",
                "--expected-version",
                "5",
                "--expected-checksum",
                str(artifact_checksum),
            )
            self.assertEqual(query.returncode, 0, query.stderr + query.stdout)
            self.assertIn("object_payload_kind=64", query.stdout)

            mismatch = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-file-mismatch",
                "--backing-len",
                str(len(object_payload) + 1),
                "--checksum",
                str(object_checksum),
                "--payload-file",
                str(object_source),
            )
            self.assertNotEqual(mismatch.returncode, 0, mismatch.stderr + mismatch.stdout)
            self.assertIn("status=checksum_mismatch", mismatch.stdout)

            ambiguous = self._run_client(
                "put-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-file-ambiguous",
                "--payload-inline",
                "inline",
                "--payload-file",
                str(object_source),
            )
            self.assertNotEqual(ambiguous.returncode, 0,
                                ambiguous.stderr + ambiguous.stdout)
            self.assertIn("status=unsupported", ambiguous.stdout)

            object_block.write_bytes(b"corrupt-file-object")
            bad_get = self._run_client(
                "get-object",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "sealed-file-object",
            )
            self.assertNotEqual(bad_get.returncode, 0, bad_get.stderr + bad_get.stdout)
            self.assertIn("status=checksum_mismatch", bad_get.stdout)
            self.assertFalse(object_block.exists())
            self.assertTrue(list(quarantine_dir.glob(f"{object_checksum:016x}.bad.*")))
        finally:
            self._stop_server(proc)

    def test_compat_matrix_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("compat-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("matrix_version=1", fixtures.stdout)
        self.assertIn("matrix_len=1978", fixtures.stdout)
        self.assertIn("matrix_checksum=0x61d07124", fixtures.stdout)
        self.assertIn("operations=23", fixtures.stdout)
        self.assertIn("fields=113", fixtures.stdout)
        self.assertIn("statuses=11", fixtures.stdout)

        matrix = self._run_client("compat-matrix")
        self.assertEqual(matrix.returncode, 0, matrix.stderr + matrix.stdout)
        self.assertEqual(matrix.stdout, COMPAT_MATRIX.read_text())

    def test_compat_baseline_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("compat-baseline-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("baseline_version=1", fixtures.stdout)
        self.assertIn("baseline_len=1251", fixtures.stdout)
        self.assertIn("baseline_checksum=0x1e017705", fixtures.stdout)
        self.assertIn("old_client_new_server=v1", fixtures.stdout)
        self.assertIn("new_client_old_server=certified", fixtures.stdout)

        baseline = self._run_client("compat-baseline-v1")
        self.assertEqual(baseline.returncode, 0, baseline.stderr + baseline.stdout)
        self.assertEqual(baseline.stdout, COMPAT_BASELINE_V1.read_text())

    def test_compat_old_new_matrix_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("compat-old-new-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("matrix_len=1733", fixtures.stdout)
        self.assertIn("matrix_checksum=0x627bf6a1", fixtures.stdout)
        self.assertIn("old_payloads=23", fixtures.stdout)
        self.assertIn("current_payloads=23", fixtures.stdout)
        self.assertIn("old_server_runtime_binary=in-tree", fixtures.stdout)

        matrix = self._run_client("compat-old-new-matrix")
        self.assertEqual(matrix.returncode, 0, matrix.stderr + matrix.stdout)
        self.assertEqual(matrix.stdout, COMPAT_OLD_NEW_MATRIX.read_text())

    def test_compat_runtime_fixtures_exercise_old_and_current_profiles(self):
        fixtures = self._run_client("compat-runtime-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("old_v1_client_current_server=runtime-compatible", fixtures.stdout)
        self.assertIn("current_v1_client_current_server=runtime-compatible",
                      fixtures.stdout)
        self.assertIn("serving_paths=object,runtime-handoff,execution-artifact",
                      fixtures.stdout)
        self.assertIn("pretraining_commits=1", fixtures.stdout)
        self.assertIn("idempotency_replay=1", fixtures.stdout)
        self.assertIn("idempotency_conflict=1", fixtures.stdout)
        self.assertIn("fail_closed=4", fixtures.stdout)
        self.assertIn("old_server_runtime_binary=in-tree", fixtures.stdout)

    def test_compat_old_server_runtime_fixtures_certify_new_client_old_server(self):
        fixtures = self._run_client("compat-old-server-runtime-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("new_client_old_server=certified", fixtures.stdout)
        self.assertIn("old_server_runtime_binary=in-tree", fixtures.stdout)
        self.assertIn("old_served_adversarial=4", fixtures.stdout)

        def _counter(name):
            token = name + "="
            for field in fixtures.stdout.split():
                if field.startswith(token):
                    return int(field[len(token):])
            self.fail(f"missing {token} counter in fixtures stdout")

        current_fail_closed = _counter("current_fail_closed")
        old_fail_closed = _counter("old_fail_closed")
        # Load-bearing contrast: the current server fail-closes on every
        # adversarial extended-profile query, while the old-server variant
        # (enforce_expected_context=false) serves them. This cannot be faked
        # by canned strings -- the counters come from the real metrics path.
        self.assertEqual(current_fail_closed, 4)
        self.assertEqual(old_fail_closed, 0)
        self.assertLess(old_fail_closed, current_fail_closed)
        self.assertEqual(_counter("current_invalid_model_binding"), 1)
        self.assertEqual(_counter("current_stale_ref"), 1)
        self.assertEqual(_counter("current_checksum_mismatch"), 1)

    def test_serving_fail_closed_fixtures_cover_mismatch_matrix(self):
        fixtures = self._run_client("serving-fail-closed-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("serving_fail_closed_matrix=certified", fixtures.stdout)
        self.assertIn("serving_paths=runtime-handoff,execution-artifact", fixtures.stdout)
        # Full mismatch matrix across both serving artifact types (runtime-handoff
        # + execution-artifact), each fail-closing on session/model/kind/owner/
        # version/checksum. Counters come from the real metrics path, not canned
        # strings.
        self.assertIn(
            "mismatch_cases=invalid-session,invalid-model-binding,invalid-owner,stale-ref,checksum-mismatch",
            fixtures.stdout,
        )
        self.assertIn("invalid_session=2", fixtures.stdout)
        self.assertIn("invalid_model_binding=4", fixtures.stdout)
        self.assertIn("stale_ref=4", fixtures.stdout)
        self.assertIn("checksum_mismatch=2", fixtures.stdout)
        self.assertIn("fail_closed=12", fixtures.stdout)

    def test_pretraining_fail_closed_fixtures_cover_mismatch_matrix(self):
        fixtures = self._run_client("pretraining-fail-closed-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("pretraining_fail_closed_matrix=certified", fixtures.stdout)
        self.assertIn("pretraining_paths=training-step-commit", fixtures.stdout)
        self.assertIn(
            "mismatch_cases=invalid-session,invalid-model-binding,invalid-owner,stale-ref,checksum-mismatch",
            fixtures.stdout,
        )
        self.assertIn("invalid_session=1", fixtures.stdout)
        self.assertIn("invalid_model_binding=2", fixtures.stdout)
        self.assertIn("stale_ref=2", fixtures.stdout)
        self.assertIn("checksum_mismatch=1", fixtures.stdout)
        self.assertIn("fail_closed=6", fixtures.stdout)

    def test_typed_payload_fixtures_round_trip_and_version_gate(self):
        fixtures = self._run_client("typed-payload-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("wire_payload_typed_binary_format=typed-binary-v1",
                      fixtures.stdout)
        self.assertIn("wire_payload_text_kv_format=text-kv", fixtures.stdout)
        # Real byte-level encode/decode round-trip across all three typed field
        # kinds (string/u32/u64 incl. a >2^63 value), plus a forward-compat
        # version gate and malformed-input fail-closed -- not canned strings.
        self.assertIn("round_trip_fields=3", fixtures.stdout)
        self.assertIn("version_gate=reject-unknown-future", fixtures.stdout)
        self.assertIn("malformed_input=fail-closed", fixtures.stdout)

    def test_journal_fixtures_cli_recovers_idempotency_and_audit(self):
        fixtures = self._run_client("journal-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("journal_magic=mem_service_journal_v1", fixtures.stdout)
        self.assertIn("loaded_audit_events=1", fixtures.stdout)
        self.assertIn("replay_audit_events=2", fixtures.stdout)
        self.assertIn("idempotency_replay=1", fixtures.stdout)

    def test_journal_torn_recovery_fixtures_cli_drops_incomplete_trailing_record(self):
        fixtures = self._run_client("journal-torn-recovery-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("torn_recovery=ok", fixtures.stdout)
        self.assertIn("journal_magic=mem_service_journal_v1", fixtures.stdout)
        # The torn trailing frame is dropped and the complete prior frame is
        # replayable, proving crash-safe load against a real on-disk torn file.
        self.assertIn("idempotency_replay=1", fixtures.stdout)
        self.assertIn("atomic_append_barrier=fsync", fixtures.stdout)

    def test_journal_compaction_fixtures_cli_compacts_journal(self):
        fixtures = self._run_client("journal-compaction-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("journal_compaction=1", fixtures.stdout)
        self.assertIn("journal_magic=mem_service_journal_v1", fixtures.stdout)
        self.assertIn("journal_compaction=1", fixtures.stdout)
        self.assertIn("idempotency_replay=1", fixtures.stdout)

    def test_wire_schema_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("wire-schema-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("manifest_len=9416", fixtures.stdout)
        self.assertIn("manifest_checksum=0xf4cf34c6", fixtures.stdout)
        self.assertIn("operations=23", fixtures.stdout)
        self.assertIn("fields=113", fixtures.stdout)

        manifest = self._run_client("wire-schema")
        self.assertEqual(manifest.returncode, 0, manifest.stderr + manifest.stdout)
        self.assertEqual(manifest.stdout, WIRE_SCHEMA_MANIFEST.read_text())

    def test_typed_client_api_round_trips_without_linking_daemon_or_core(self):
        proc = self._start_server()
        try:
            result = subprocess.run(
                [str(self.client_binary), f"unix:{self.socket}"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("typed_client_roundtrip=ok", result.stdout)
        finally:
            self._stop_server(proc)

    def test_pretraining_workers_publish_resolve_and_recover_refs(self):
        worker = self._compile_pretraining_worker_binary()
        first = self._start_server()
        try:
            worker0 = self._run_pretraining_worker(worker, "worker0")
            self.assertEqual(worker0.returncode, 0, worker0.stderr + worker0.stdout)
            self.assertIn("pretraining_worker=worker0 ok", worker0.stdout)

            worker1 = self._run_pretraining_worker(worker, "worker1")
            self.assertEqual(worker1.returncode, 0, worker1.stderr + worker1.stdout)
            self.assertIn("pretraining_worker=worker1 ok", worker1.stdout)

            commit_step = self._run_pretraining_worker(worker, "commit-step")
            self.assertEqual(commit_step.returncode,
                             0,
                             commit_step.stderr + commit_step.stdout)
            self.assertIn("pretraining_worker=commit-step ok", commit_step.stdout)

            resolved = self._run_pretraining_worker(worker, "resolve")
            self.assertEqual(resolved.returncode, 0, resolved.stderr + resolved.stdout)
            self.assertIn("pretraining_worker=resolve ok", resolved.stdout)

            bad_version = self._run_pretraining_worker(worker, "bad-version")
            self.assertEqual(bad_version.returncode,
                             0,
                             bad_version.stderr + bad_version.stdout)
            self.assertIn("pretraining_worker=bad-version ok", bad_version.stdout)

            bad_checksum = self._run_pretraining_worker(worker, "bad-checksum")
            self.assertEqual(bad_checksum.returncode,
                             0,
                             bad_checksum.stderr + bad_checksum.stdout)
            self.assertIn("pretraining_worker=bad-checksum ok", bad_checksum.stdout)

            step_bad_version = self._run_pretraining_worker(worker, "step-bad-version")
            self.assertEqual(step_bad_version.returncode,
                             0,
                             step_bad_version.stderr + step_bad_version.stdout)
            self.assertIn("pretraining_worker=step-bad-version ok",
                          step_bad_version.stdout)

            step_bad_checksum = self._run_pretraining_worker(worker, "step-bad-checksum")
            self.assertEqual(step_bad_checksum.returncode,
                             0,
                             step_bad_checksum.stderr + step_bad_checksum.stdout)
            self.assertIn("pretraining_worker=step-bad-checksum ok",
                          step_bad_checksum.stdout)

            records = self._run_client("list-records", "--connect", f"unix:{self.socket}")
            self.assertEqual(records.returncode, 0, records.stderr + records.stdout)
            self.assertIn("kind_name=training_artifact", records.stdout)
            self.assertIn("key=training/run-b/worker-0/dataset-shard-0000",
                          records.stdout)
            self.assertIn("key=training/run-b/worker-1/gradient-bucket-0010",
                          records.stdout)
            self.assertIn("key=training/run-b/checkpoint-0010", records.stdout)
            self.assertIn("key=training/run-b/global-step-0010/commit",
                          records.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            parsed_metrics = self._parse_metrics(metrics.stdout)
            self.assertEqual(parsed_metrics["register_training_artifact_count"], 6)
            self.assertEqual(parsed_metrics["query_training_artifact_count"], 10)
            self.assertEqual(parsed_metrics["stale_ref_count"], 2)
            self.assertEqual(parsed_metrics["checksum_mismatch_count"], 2)
            self.assertEqual(parsed_metrics["fail_closed_count"], 4)
        finally:
            self._stop_server(first)

        store_text = self.store.read_text()
        self.assertIn("record_count=6", store_text)
        self.assertIn("key=training/run-b/worker-0/dataset-shard-0000", store_text)
        self.assertIn("key=training/run-b/worker-1/sample-batch-0010", store_text)
        self.assertIn("key=training/run-b/worker-1/gradient-bucket-0010", store_text)
        self.assertIn("key=training/run-b/checkpoint-0010", store_text)
        self.assertIn("key=training/run-b/worker-1/optimizer-state-0010", store_text)
        self.assertIn("key=training/run-b/global-step-0010/commit", store_text)
        self.assertIn("artifact_kind=training-step-commit", store_text)
        self.assertIn("key=pretrain/run-b/w1/gradient/v3", store_text)
        self.assertIn("key=pretrain/run-b/global-step-10/commit/v6", store_text)
        self.assertIn("status=0", store_text)
        self.assertIn("response_line=artifact_kind=gradient-bucket", store_text)
        self.assertIn("response_line=artifact_kind=training-step-commit", store_text)

        second = self._start_server()
        try:
            resolved_after_restart = self._run_pretraining_worker(worker, "resolve")
            self.assertEqual(resolved_after_restart.returncode,
                             0,
                             resolved_after_restart.stderr + resolved_after_restart.stdout)
            self.assertIn("pretraining_worker=resolve ok",
                          resolved_after_restart.stdout)

            conflict = self._run_pretraining_worker(worker, "conflict")
            self.assertEqual(conflict.returncode, 0, conflict.stderr + conflict.stdout)
            self.assertIn("pretraining_worker=conflict ok", conflict.stdout)

            step_conflict = self._run_pretraining_worker(worker, "step-conflict")
            self.assertEqual(step_conflict.returncode,
                             0,
                             step_conflict.stderr + step_conflict.stdout)
            self.assertIn("pretraining_worker=step-conflict ok", step_conflict.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            parsed_metrics = self._parse_metrics(metrics.stdout)
            self.assertEqual(parsed_metrics["query_training_artifact_count"], 8)
            self.assertEqual(parsed_metrics["version_conflict_count"], 2)
            self.assertEqual(parsed_metrics["idempotency_conflict_count"], 2)
            self.assertEqual(parsed_metrics["fail_closed_count"], 2)
        finally:
            self._stop_server(second)

    def test_artifact_query_binding_mismatch_fails_closed(self):
        proc = self._start_server()
        try:
            publish = self._run_client(
                "publish-runtime-handoff",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "runtime/session-a/range-0",
                "--session-id",
                "session-a",
                "--model-key",
                "model-a",
                "--artifact-kind",
                "hidden-range",
                "--artifact-id",
                "range-0",
                "--checksum",
                "1111",
                "--version",
                "7",
            )
            self.assertEqual(publish.returncode, 0, publish.stderr + publish.stdout)
            self.assertIn("status=ok", publish.stdout)

            bad_session = self._run_client(
                "resolve-runtime-handoff",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "runtime/session-a/range-0",
                "--expected-session-id",
                "session-b",
                "--expected-model-key",
                "model-a",
                "--expected-artifact-kind",
                "hidden-range",
                "--expected-artifact-id",
                "range-0",
                "--expected-version",
                "7",
                "--expected-checksum",
                "1111",
            )
            self.assertNotEqual(bad_session.returncode, 0, bad_session.stdout)
            self.assertIn("status=invalid_session", bad_session.stdout)

            bad_model = self._run_client(
                "resolve-runtime-handoff",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "runtime/session-a/range-0",
                "--expected-session-id",
                "session-a",
                "--expected-model-key",
                "model-b",
                "--expected-artifact-kind",
                "hidden-range",
                "--expected-artifact-id",
                "range-0",
                "--expected-version",
                "7",
                "--expected-checksum",
                "1111",
            )
            self.assertNotEqual(bad_model.returncode, 0, bad_model.stdout)
            self.assertIn("status=invalid_model_binding", bad_model.stdout)

            bad_kind = self._run_client(
                "resolve-runtime-handoff",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "runtime/session-a/range-0",
                "--expected-session-id",
                "session-a",
                "--expected-model-key",
                "model-a",
                "--expected-artifact-kind",
                "logits",
                "--expected-artifact-id",
                "range-0",
                "--expected-version",
                "7",
                "--expected-checksum",
                "1111",
            )
            self.assertNotEqual(bad_kind.returncode, 0, bad_kind.stdout)
            self.assertIn("status=stale_ref", bad_kind.stdout)

            bad_id = self._run_client(
                "resolve-runtime-handoff",
                "--connect",
                f"unix:{self.socket}",
                "--key",
                "runtime/session-a/range-0",
                "--expected-session-id",
                "session-a",
                "--expected-model-key",
                "model-a",
                "--expected-artifact-kind",
                "hidden-range",
                "--expected-artifact-id",
                "range-1",
                "--expected-version",
                "7",
                "--expected-checksum",
                "1111",
            )
            self.assertNotEqual(bad_id.returncode, 0, bad_id.stdout)
            self.assertIn("status=stale_ref", bad_id.stdout)

            metrics = self._run_client("metrics", "--connect", f"unix:{self.socket}")
            self.assertEqual(metrics.returncode, 0, metrics.stderr + metrics.stdout)
            self.assertIn("status=ok", metrics.stdout)
            self.assertIn("publish_runtime_handoff_count=1", metrics.stdout)
            self.assertIn("resolve_runtime_handoff_count=4", metrics.stdout)
            self.assertIn("invalid_session_count=1", metrics.stdout)
            self.assertIn("invalid_model_binding_count=1", metrics.stdout)
            self.assertIn("stale_ref_count=2", metrics.stdout)
            self.assertIn("fail_closed_count=4", metrics.stdout)
        finally:
            self._stop_server(proc)

    def test_serving_and_pretraining_sdk_examples_round_trip(self):
        serving = self._compile_sdk_example("mem_service_serving_example.c",
                                            "mem_service_serving_example")
        pretraining = self._compile_sdk_example("mem_service_pretraining_example.c",
                                                "mem_service_pretraining_example")
        proc = self._start_server()
        try:
            serving_result = subprocess.run(
                [str(serving), f"unix:{self.socket}"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(serving_result.returncode,
                             0,
                             serving_result.stderr + serving_result.stdout)
            self.assertIn("mem_service_serving_example=ok", serving_result.stdout)
            self.assertIn("runtime_version=7", serving_result.stdout)
            self.assertIn("logits_version=8", serving_result.stdout)

            pretraining_result = subprocess.run(
                [str(pretraining), f"unix:{self.socket}"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(pretraining_result.returncode,
                             0,
                             pretraining_result.stderr + pretraining_result.stdout)
            self.assertIn("mem_service_pretraining_example=ok",
                          pretraining_result.stdout)
            self.assertIn("artifacts=6", pretraining_result.stdout)
            self.assertIn("last_kind=training-step-commit", pretraining_result.stdout)
        finally:
            self._stop_server(proc)


@unittest.skipUnless(shutil.which("cc") and shutil.which("make"), "host cc and make are required")
class MemServiceReleaseInstallTests(unittest.TestCase):
    def _free_tcp_port(self) -> int:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listener.bind(("127.0.0.1", 0))
            return int(listener.getsockname()[1])
        finally:
            listener.close()

    def _http_metrics_request(self, port: int) -> str:
        request = (
            "GET /metrics HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).encode()
        with socket.create_connection(("127.0.0.1", port), timeout=2.0) as conn:
            conn.sendall(request)
            chunks = []
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                chunks.append(chunk)
        return b"".join(chunks).decode()

    def _collect_prometheus_metrics(self, response: str) -> dict[str, int]:
        self.assertIn("HTTP/1.1 200 OK\r\n", response)
        self.assertIn("Content-Type: text/plain; version=0.0.4\r\n", response)
        self.assertIn("Cache-Control: no-store\r\n", response)
        _, body = response.split("\r\n\r\n", 1)
        metrics: dict[str, int] = {}
        types: dict[str, str] = {}

        for line in body.splitlines():
            if not line:
                continue
            if line.startswith("# TYPE "):
                _, _, name, metric_type = line.split(" ", 3)
                types[name] = metric_type
                continue
            if line.startswith("#"):
                continue
            name, value = line.split(" ", 1)
            metrics[name] = int(value)

        self.assertEqual(types["lingqu_mem_service_request_count"], "counter")
        self.assertEqual(
            types["lingqu_mem_service_request_latency_max_ms"],
            "gauge",
        )
        return metrics

    def _install_release_layout(self, app_dir: Path, destdir: Path) -> None:
        cmd = [
            "make",
            "-C",
            str(app_dir),
            "CC=cc",
            "CFLAGS=-O2 -Wall -Wextra",
            f"DESTDIR={destdir}",
            "PREFIX=/usr",
            "install-smoke",
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _run_installed_sdk_example_smoke(
        self,
        app_dir: Path,
        destdir: Path,
        package_out: Path,
    ) -> None:
        cmd = [
            "make",
            "-C",
            str(app_dir),
            "CC=cc",
            "CFLAGS=-O2 -Wall -Wextra",
            f"DESTDIR={destdir}",
            f"PACKAGE_OUT_DIR={package_out}",
            "PREFIX=/usr",
            "installed-sdk-example-smoke",
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _run_installed_sdk_pkgconfig_smoke(
        self,
        app_dir: Path,
        destdir: Path,
        package_out: Path,
    ) -> None:
        cmd = [
            "make",
            "-C",
            str(app_dir),
            "CC=cc",
            "CFLAGS=-O2 -Wall -Wextra",
            f"DESTDIR={destdir}",
            f"PACKAGE_OUT_DIR={package_out}",
            "PREFIX=/usr",
            "installed-sdk-pkgconfig-smoke",
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _run_installed_sdk_runtime_smoke(
        self,
        app_dir: Path,
        destdir: Path,
        package_out: Path,
    ) -> subprocess.CompletedProcess:
        cmd = [
            "make",
            "-C",
            str(app_dir),
            "CC=cc",
            "CFLAGS=-O2 -Wall -Wextra",
            f"DESTDIR={destdir}",
            f"PACKAGE_OUT_DIR={package_out}",
            "PREFIX=/usr",
            "installed-sdk-runtime-smoke",
        ]
        return subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )

    def _parse_exec_start(self, service_unit: Path) -> list[str]:
        for line in service_unit.read_text().splitlines():
            if line.startswith("ExecStart="):
                return shlex.split(line.split("=", 1)[1])
        self.fail(f"{service_unit} has no ExecStart")

    def _wait_installed_service_ready(
        self,
        proc: subprocess.Popen,
        client_binary: Path,
        socket_path: Path,
    ) -> None:
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate(timeout=1)
                if "Operation not permitted" in stderr and "mem_service serve: bind" in stderr:
                    raise unittest.SkipTest("sandbox forbids Unix socket bind in subprocess")
                if (
                    "Operation not permitted" in stderr
                    and "mem_service serve: metrics bind" in stderr
                ):
                    raise unittest.SkipTest("sandbox forbids TCP metrics bind in subprocess")
                self.fail(
                    f"installed mem_service daemon exited rc={proc.returncode}\n"
                    f"stdout={stdout}\nstderr={stderr}"
                )
            health = subprocess.run(
                [str(client_binary), "health", "--connect", f"unix:{socket_path}"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            if health.returncode == 0 and "status=ok" in health.stdout:
                return
            time.sleep(0.05)
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=5)
        self.fail(
            "installed mem_service daemon did not become ready\n"
            f"stdout={stdout}\nstderr={stderr}"
        )

    def test_make_install_smoke_creates_release_layout(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_install_", dir=str(_tmp_parent())) as tmp:
            destdir = Path(tmp)
            try:
                self._install_release_layout(app_dir, destdir)
            finally:
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )

            self.assertTrue((destdir / "usr" / "bin" / "linqu_mem_service").exists())
            self.assertTrue(
                (
                    destdir
                    / "usr"
                    / "libexec"
                    / "lingqu"
                    / "mem_service"
                    / "linqu_mem_service_host"
                ).exists()
            )
            self.assertTrue(
                (destdir / "usr" / "include" / "lingqu" / "mem_service" / "mem_service_client.h").exists()
            )
            self.assertTrue(
                (destdir / "usr" / "src" / "lingqu" / "mem_service" / "mem_service_client.c").exists()
            )
            self.assertTrue(
                (
                    destdir
                    / "usr"
                    / "share"
                    / "lingqu"
                    / "mem_service"
                    / "examples"
                    / "mem_service_serving_example.c"
                ).exists()
            )
            self.assertTrue(
                (
                    destdir
                    / "usr"
                    / "share"
                    / "lingqu"
                    / "mem_service"
                    / "examples"
                    / "mem_service_pretraining_example.c"
                ).exists()
            )
            manifest = (
                destdir / "usr" / "share" / "lingqu" / "mem_service" / "release-manifest.txt"
            )
            package_manifest = (
                destdir / "usr" / "share" / "lingqu" / "mem_service" / "package-manifest.txt"
            )
            pkgconfig = destdir / "usr" / "lib" / "pkgconfig" / "lingqu-mem-service.pc"
            wire_schema = destdir / "usr" / "share" / "lingqu" / "mem_service" / "wire-schema.txt"
            admin_output_schema = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "admin-output-schema.txt"
            )
            upgrade_rollback_policy = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "upgrade-rollback-policy.txt"
            )
            ops_certification_policy = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "ops-certification-policy.txt"
            )
            api_abi_policy = (
                destdir / "usr" / "share" / "lingqu" / "mem_service" / "api-abi-policy.txt"
            )
            compat_matrix = (
                destdir / "usr" / "share" / "lingqu" / "mem_service" / "compat-matrix.txt"
            )
            compat_baseline = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "compat-baseline-v1.txt"
            )
            compat_old_new_matrix = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "compat-old-new-matrix.txt"
            )
            host_deploy_manifest = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "deploy"
                / "linqu_mem_service.host.service"
            )
            alert_rules = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "deploy"
                / "linqu_mem_service.prometheus-alerts.yml"
            )
            self.assertTrue(manifest.exists())
            self.assertTrue(package_manifest.exists())
            self.assertTrue(pkgconfig.exists())
            self.assertTrue(wire_schema.exists())
            self.assertTrue(admin_output_schema.exists())
            self.assertTrue(upgrade_rollback_policy.exists())
            self.assertTrue(ops_certification_policy.exists())
            self.assertTrue(api_abi_policy.exists())
            self.assertTrue(compat_matrix.exists())
            self.assertTrue(compat_baseline.exists())
            self.assertTrue(compat_old_new_matrix.exists())
            self.assertTrue(host_deploy_manifest.exists())
            self.assertTrue(alert_rules.exists())
            self.assertIn("core_binary=bin/linqu_mem_service", manifest.read_text())
            self.assertIn(
                "host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host",
                manifest.read_text(),
            )
            self.assertIn(
                "host_daemon_artifact_smoke=host-artifact-smoke",
                manifest.read_text(),
            )
            self.assertIn("package_format=installed-layout-v1", manifest.read_text())
            self.assertIn("package_manifest_checksum=0x28945f1f", manifest.read_text())
            self.assertIn(
                "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_example_smoke=installed-sdk-example-smoke",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_example_smoke_scope=serving+pretraining-external-client-compile",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_runtime_smoke=installed-sdk-runtime-smoke",
                manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_runtime_smoke_scope=installed-host-daemon+serving+pretraining-runtime",
                manifest.read_text(),
            )
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", manifest.read_text())
            self.assertIn("pkgconfig_name=lingqu-mem-service", manifest.read_text())
            self.assertIn("pkgconfig_cflags=-I${includedir}", manifest.read_text())
            self.assertIn(
                "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c",
                manifest.read_text(),
            )
            self.assertIn("prefix=/usr", pkgconfig.read_text())
            self.assertIn("includedir=${prefix}/include/lingqu/mem_service", pkgconfig.read_text())
            self.assertIn("sourcedir=${prefix}/src/lingqu/mem_service", pkgconfig.read_text())
            self.assertIn("Cflags: -I${includedir}", pkgconfig.read_text())
            self.assertIn(
                "sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c",
                pkgconfig.read_text(),
            )
            self.assertIn("distributable_package_format=tar", manifest.read_text())
            self.assertIn(
                "distributable_package_gate=package-tarball-smoke",
                manifest.read_text(),
            )
            self.assertIn("native_package_format=deb", manifest.read_text())
            self.assertIn("native_package_arch=arm64", manifest.read_text())
            self.assertIn("native_package_gate=package-deb-smoke", manifest.read_text())
            self.assertIn(
                "native_package_runtime=not-executed-cross-compiled-arm64",
                manifest.read_text(),
            )
            self.assertIn("rpm_native_package_format=rpm", manifest.read_text())
            self.assertIn("rpm_native_package_gate=package-rpm-smoke", manifest.read_text())
            self.assertIn(
                "rpm_native_package_runtime=requires-linux-rpm-toolchain",
                manifest.read_text(),
            )
            scripts_dir = destdir / "usr" / "share" / "lingqu" / "mem_service" / "scripts"
            self.assertTrue((scripts_dir / "run_mem_service_linux_ops_ci.sh").exists())
            self.assertTrue((scripts_dir / "run_mem_service_linux_ops_ci.sh").stat().st_mode & 0o111)
            self.assertTrue(
                (scripts_dir / "verify_mem_service_release_certification.sh").exists()
            )
            self.assertTrue(
                (scripts_dir / "verify_mem_service_release_certification.sh").stat().st_mode
                & 0o111
            )
            self.assertTrue((scripts_dir / "verify_mem_service_installed_layout.sh").exists())
            self.assertTrue(
                (scripts_dir / "verify_mem_service_installed_layout.sh").stat().st_mode
                & 0o111
            )
            self.assertTrue((scripts_dir / "verify_mem_service_installed_sdk.sh").exists())
            self.assertTrue(
                (scripts_dir / "verify_mem_service_installed_sdk.sh").stat().st_mode
                & 0o111
            )
            self.assertTrue((scripts_dir / "run_mem_service_release_certification_ci.sh").exists())
            self.assertTrue(
                (scripts_dir / "run_mem_service_release_certification_ci.sh").stat().st_mode
                & 0o111
            )
            installed_host = (
                destdir
                / "usr"
                / "libexec"
                / "lingqu"
                / "mem_service"
                / "linqu_mem_service_host"
            )
            ops_dry_run = subprocess.run(
                [
                    str(scripts_dir / "verify_mem_service_linux_ops_evidence.sh"),
                    "--evidence-file",
                    "/tmp/linqu_mem_service_ops.evidence",
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            remote_dry_run = subprocess.run(
                [
                    str(scripts_dir / "verify_mem_service_remote_transport_evidence.sh"),
                    "--evidence-file",
                    "/tmp/linqu_mem_service_remote_transport.evidence",
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            remote_ci_dry_run = subprocess.run(
                [
                    str(scripts_dir / "run_mem_service_remote_transport_ci.sh"),
                    "--source",
                    "tcp:10.0.0.11:9000",
                    "--producer-host",
                    "producer-a",
                    "--consumer-host",
                    "consumer-b",
                    "--network-partition-marker",
                    "/tmp/remote-transport.partition",
                    "--out-dir",
                    str(destdir / "remote-transport-ci"),
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            linux_ops_ci_dry_run = subprocess.run(
                [
                    str(scripts_dir / "run_mem_service_linux_ops_ci.sh"),
                    "--rollback-rpm",
                    "/tmp/linqu-mem-service-prev.rpm",
                    "--rpm-file",
                    "/tmp/linqu-mem-service-current.rpm",
                    "--out-dir",
                    str(destdir / "linux-ops-ci"),
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            installed_layout = subprocess.run(
                [
                    str(scripts_dir / "verify_mem_service_installed_layout.sh"),
                    "--no-runtime",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            installed_sdk_dry_run = subprocess.run(
                [
                    str(scripts_dir / "verify_mem_service_installed_sdk.sh"),
                    "--work-dir",
                    str(destdir / "sdk-work"),
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            installed_sdk_preflight_dry_run = subprocess.run(
                [
                    str(scripts_dir / "verify_mem_service_installed_sdk.sh"),
                    "--work-dir",
                    str(destdir / "sdk-work"),
                    "--preflight",
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            release_ci_dry_run = subprocess.run(
                [
                    str(scripts_dir / "run_mem_service_release_certification_ci.sh"),
                    "--rollback-rpm",
                    "/tmp/linqu-mem-service-prev.rpm",
                    "--rpm-file",
                    "/tmp/linqu-mem-service-current.rpm",
                    "--source",
                    "tcp:10.0.0.11:9000",
                    "--producer-host",
                    "producer-a",
                    "--consumer-host",
                    "consumer-b",
                    "--network-partition-marker",
                    "/tmp/remote-transport.partition",
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            release_preflight_dry_run = subprocess.run(
                [
                    str(scripts_dir / "run_mem_service_release_certification_ci.sh"),
                    "--rollback-rpm",
                    "/tmp/linqu-mem-service-prev.rpm",
                    "--rpm-file",
                    "/tmp/linqu-mem-service-current.rpm",
                    "--source",
                    "tcp:10.0.0.11:9000",
                    "--producer-host",
                    "producer-a",
                    "--consumer-host",
                    "consumer-b",
                    "--network-partition-marker",
                    "/tmp/remote-transport.partition",
                    "--preflight",
                    "--dry-run",
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            self.assertIn(
                f"{installed_host} ops-certification-verify",
                ops_dry_run.stdout,
            )
            self.assertIn(
                f"{installed_host} remote-transport-verify",
                remote_dry_run.stdout,
            )
            self.assertIn(
                f"{installed_host} remote-transport-generate-evidence",
                remote_ci_dry_run.stdout,
            )
            self.assertIn(
                "verify_mem_service_remote_transport_bundle.sh --bundle-file",
                remote_ci_dry_run.stdout,
            )
            self.assertNotIn("/share/lingqu/mem_service/apps/mem_service", remote_ci_dry_run.stdout)
            self.assertIn(
                f"{installed_host} ops-certification-linux-ci-smoke",
                linux_ops_ci_dry_run.stdout,
            )
            self.assertIn(
                "verify_mem_service_ops_certification_bundle.sh --bundle-file",
                linux_ops_ci_dry_run.stdout,
            )
            self.assertNotIn("/share/lingqu/mem_service/apps/mem_service", linux_ops_ci_dry_run.stdout)
            self.assertIn("[mem-service-installed-layout] PASS", installed_layout.stdout)
            self.assertIn(
                "pkg-config --define-prefix --exists lingqu-mem-service",
                installed_sdk_dry_run.stdout,
            )
            self.assertIn(
                "pkg-config --define-prefix --variable=sdk_sources lingqu-mem-service",
                installed_sdk_preflight_dry_run.stdout,
            )
            self.assertIn(
                "verify_mem_service_release_certification.sh --ops-bundle-file",
                release_ci_dry_run.stdout,
            )
            self.assertIn(
                "verify_mem_service_installed_sdk.sh --work-dir",
                release_ci_dry_run.stdout,
            )
            self.assertIn(
                "--rpm-file /tmp/linqu-mem-service-current.rpm",
                release_ci_dry_run.stdout,
            )
            self.assertNotIn("/share/lingqu/mem_service/apps/mem_service", release_ci_dry_run.stdout)
            self.assertIn(
                "verify_mem_service_installed_sdk.sh --work-dir",
                release_preflight_dry_run.stdout,
            )
            self.assertIn(
                "--preflight",
                release_preflight_dry_run.stdout,
            )
            self.assertIn(
                "run_mem_service_linux_ops_ci.sh --rollback-rpm "
                "/tmp/linqu-mem-service-prev.rpm --rpm-file "
                "/tmp/linqu-mem-service-current.rpm",
                release_preflight_dry_run.stdout,
            )
            self.assertIn(
                "run_mem_service_remote_transport_ci.sh --source "
                "tcp:10.0.0.11:9000",
                release_preflight_dry_run.stdout,
            )
            self.assertNotIn(
                "/share/lingqu/mem_service/apps/mem_service",
                release_preflight_dry_run.stdout,
            )
            self.assertIn(
                "release_script_root=share/lingqu/mem_service/scripts",
                manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "verify_mem_service_release_certification.sh",
                manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "verify_mem_service_installed_layout.sh",
                manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "verify_mem_service_installed_sdk.sh",
                manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "run_mem_service_release_certification_ci.sh",
                manifest.read_text(),
            )
            self.assertIn(
                "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file",
                manifest.read_text(),
            )
            self.assertIn("package_gate=package-fixtures", manifest.read_text())
            self.assertIn("artifact_format=tar", package_manifest.read_text())
            self.assertIn(
                "artifact_name=linqu_mem_service-installed-layout-v1.tar",
                package_manifest.read_text(),
            )
            self.assertIn(
                "artifact_gate=package-tarball-smoke",
                package_manifest.read_text(),
            )
            self.assertIn("native_package_format=deb", package_manifest.read_text())
            self.assertIn(
                "native_package_name=linqu-mem-service_0.1.0-1_arm64.deb",
                package_manifest.read_text(),
            )
            self.assertIn("native_package_arch=arm64", package_manifest.read_text())
            self.assertIn(
                "native_package_gate=package-deb-smoke",
                package_manifest.read_text(),
            )
            self.assertIn("rpm_package_format=rpm", package_manifest.read_text())
            self.assertIn("rpm_package_gate=package-rpm-smoke", package_manifest.read_text())
            self.assertIn(
                "rpm_package_runtime=requires-linux-rpm-toolchain",
                package_manifest.read_text(),
            )
            self.assertIn("installed_file_count=46", package_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", package_manifest.read_text())
            self.assertIn("pkgconfig_name=lingqu-mem-service", package_manifest.read_text())
            self.assertIn("pkgconfig_cflags=-I${includedir}", package_manifest.read_text())
            self.assertIn(
                "pkgconfig_sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_script_root=share/lingqu/mem_service/scripts",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_certification_ci=scripts/run_mem_service_release_certification_ci.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_certification_preflight=scripts/run_mem_service_release_certification_ci.sh --preflight",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_certification_readiness_gate=release-readiness --ops-evidence-file --remote-transport-evidence-file",
                package_manifest.read_text(),
            )
            self.assertIn(
                "linux_ops_ci=scripts/run_mem_service_linux_ops_ci.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "linux_ops_ci_preflight=scripts/run_mem_service_linux_ops_ci.sh --preflight",
                package_manifest.read_text(),
            )
            self.assertIn(
                "remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "remote_payload_production_transport_ci_preflight=scripts/run_mem_service_remote_transport_ci.sh --preflight",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "verify_mem_service_release_certification.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "verify_mem_service_installed_layout.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "verify_mem_service_installed_sdk.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "release_script=share/lingqu/mem_service/scripts/"
                "run_mem_service_release_certification_ci.sh",
                package_manifest.read_text(),
            )
            self.assertIn(
                "file_class=release_scripts count=10",
                package_manifest.read_text(),
            )
            self.assertIn(
                "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf",
                package_manifest.read_text(),
            )
            self.assertIn(
                "runtime_config=etc/lingqu/mem_service/mem_service.conf",
                package_manifest.read_text(),
            )
            self.assertIn(
                "host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf",
                package_manifest.read_text(),
            )
            self.assertIn(
                "host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf",
                package_manifest.read_text(),
            )
            self.assertIn(
                "systemd_unit=lib/systemd/system/linqu_mem_service.service",
                package_manifest.read_text(),
            )
            self.assertIn(
                "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service",
                package_manifest.read_text(),
            )
            self.assertIn("required_gate=package-fixtures", package_manifest.read_text())
            self.assertIn(
                "required_gate=package-tarball-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "required_gate=package-deb-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "required_gate=package-rpm-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "required_gate=installed-sdk-example-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "required_gate=installed-sdk-pkgconfig-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "required_gate=installed-sdk-runtime-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_preflight=scripts/verify_mem_service_installed_sdk.sh --preflight",
                package_manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_preflight_scope=pkg-config-cflags+sdk-sources+examples+host-binary-no-compile",
                package_manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_pkgconfig_smoke=installed-sdk-pkgconfig-smoke",
                package_manifest.read_text(),
            )
            self.assertIn(
                "installed_sdk_pkgconfig_smoke_scope=pkg-config-cflags+sdk-sources-external-client-compile",
                package_manifest.read_text(),
            )
            self.assertIn(
                "required_gate=ops-certification-fixtures",
                package_manifest.read_text(),
            )
            self.assertIn(
                "contract=ops-certification-policy",
                package_manifest.read_text(),
            )
            self.assertIn("cross_version_upgrade=certified", package_manifest.read_text())
            self.assertIn("wire_schema_manifest_checksum=0xf4cf34c6", manifest.read_text())
            self.assertIn("admin_output_schema_checksum=0x7021f4cf", manifest.read_text())
            self.assertIn("admin_output_format=text-kv", manifest.read_text())
            self.assertIn("admin_metric_prefix=lingqu_mem_service_", manifest.read_text())
            self.assertIn("upgrade_rollback_policy_checksum=0xf7943816", manifest.read_text())
            self.assertIn("upgrade_policy=current-version-only", manifest.read_text())
            self.assertIn("rollback_policy=current-version-only", manifest.read_text())
            self.assertIn("old_server_runtime_binary=certified", manifest.read_text())
            self.assertIn("alert_rules_checksum=0x05a9245c", manifest.read_text())
            self.assertIn("alert_rule_count=6", manifest.read_text())
            self.assertIn("alert_rules_gate=alert-fixtures", manifest.read_text())
            self.assertIn(
                "alert_integration_smoke=alert-integration-fixtures",
                manifest.read_text(),
            )
            self.assertIn(
                "ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt",
                manifest.read_text(),
            )
            self.assertIn("ops_certification_policy_checksum=0xe77c644b", manifest.read_text())
            self.assertIn("ops_certification_gate=ops-certification-fixtures",
                          manifest.read_text())
            self.assertIn("ops_certification_evidence_gate=ops-certification-evidence-fixtures",
                          manifest.read_text())
            self.assertIn("ops_certification_generate=ops-certification-generate-evidence",
                          manifest.read_text())
            self.assertIn("ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke",
                          manifest.read_text())
            self.assertIn("linux_ops_certification_smoke=linux-ops-certification-smoke",
                          manifest.read_text())
            self.assertIn("linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke",
                          manifest.read_text())
            self.assertIn("linux_ops_deployment_smoke=linux-ops-deployment-smoke",
                          manifest.read_text())
            self.assertIn("ops_certification_verify=ops-certification-verify --evidence-file",
                          manifest.read_text())
            self.assertIn("real_systemd_environment=not-certified", manifest.read_text())
            self.assertIn("production_collector_alert_environment=not-certified",
                          manifest.read_text())
            self.assertIn("rpm_package=not-certified", manifest.read_text())
            self.assertIn("api_abi_policy_checksum=0x5d95ae02", manifest.read_text())
            self.assertIn("client_api_version=1", manifest.read_text())
            self.assertIn("client_abi_version=1", manifest.read_text())
            self.assertIn("client_record_abi_size=744", manifest.read_text())
            self.assertIn("compat_runtime_gate=compat-runtime-fixtures",
                          manifest.read_text())
            self.assertIn("serving_fail_closed_matrix=certified",
                          manifest.read_text())
            self.assertIn("serving_fail_closed_gate=serving-fail-closed-fixtures",
                          manifest.read_text())
            self.assertIn("pretraining_fail_closed_matrix=certified",
                          manifest.read_text())
            self.assertIn(
                "pretraining_fail_closed_gate=pretraining-fail-closed-fixtures",
                manifest.read_text())
            self.assertIn("wire_payload_text_kv_format=text-kv",
                          manifest.read_text())
            self.assertIn("wire_payload_typed_binary_format=typed-binary-v1",
                          manifest.read_text())
            self.assertIn(
                "wire_payload_typed_binary_gate=typed-payload-fixtures",
                manifest.read_text())
            self.assertIn("compat_matrix_checksum=0x61d07124", manifest.read_text())
            self.assertIn("compat_baseline_checksum=0x1e017705", manifest.read_text())
            self.assertIn("compat_old_new_matrix_checksum=0x627bf6a1",
                          manifest.read_text())
            self.assertIn(
                "host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service",
                manifest.read_text(),
            )
            self.assertIn(
                "host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf",
                manifest.read_text(),
            )
            self.assertIn(
                "host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf",
                manifest.read_text(),
            )
            self.assertIn(
                "systemd_unit=lib/systemd/system/linqu_mem_service.service",
                manifest.read_text(),
            )
            self.assertIn(
                "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service",
                manifest.read_text(),
            )
            self.assertIn(
                "host_service_manager_smoke=installed-host-service-manager-smoke",
                manifest.read_text(),
            )
            self.assertIn(
                "host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm",
                manifest.read_text(),
            )
            self.assertIn("durable_backend=snapshot+journal", manifest.read_text())
            self.assertIn("durable_catalog=storage-root-v1", manifest.read_text())
            self.assertIn("durable_catalog_manifest=catalog/manifest.txt", manifest.read_text())
            self.assertIn("payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1",
                          manifest.read_text())
            self.assertIn("durable_journal=store-path.journal", manifest.read_text())
            self.assertIn("deployment_smoke=deployment-fixtures", manifest.read_text())
            self.assertIn(
                "service_manager_lifecycle=serve-config-ready-scrape-sigterm",
                manifest.read_text(),
            )
            self.assertIn("service_manager_shutdown=signal-clean-stop", manifest.read_text())
            self.assertIn("metrics_listen_config=metrics_listen", manifest.read_text())
            self.assertIn("metrics_http_listener=tcp-ipv4", manifest.read_text())
            self.assertIn("metrics_scrape_path=/metrics", manifest.read_text())
            self.assertIn("mem_service_serving_example.c", manifest.read_text())
            self.assertIn("mem_service_pretraining_example.c", manifest.read_text())
            self.assertIn(
                "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
                "serve --config /etc/lingqu/mem_service/mem_service.host.conf",
                host_deploy_manifest.read_text(),
            )
            self.assertEqual(wire_schema.read_text(), WIRE_SCHEMA_MANIFEST.read_text())
            self.assertEqual(admin_output_schema.read_text(), ADMIN_OUTPUT_SCHEMA.read_text())
            self.assertEqual(
                upgrade_rollback_policy.read_text(),
                UPGRADE_ROLLBACK_POLICY.read_text(),
            )
            self.assertEqual(
                ops_certification_policy.read_text(),
                OPS_CERTIFICATION_POLICY.read_text(),
            )
            self.assertEqual(alert_rules.read_text(), ALERT_RULES.read_text())
            self.assertEqual(api_abi_policy.read_text(), API_ABI_POLICY.read_text())
            self.assertEqual(compat_matrix.read_text(), COMPAT_MATRIX.read_text())
            self.assertEqual(compat_baseline.read_text(), COMPAT_BASELINE_V1.read_text())
            self.assertEqual(
                compat_old_new_matrix.read_text(),
                COMPAT_OLD_NEW_MATRIX.read_text(),
            )

    def test_installed_sdk_example_smoke_builds_external_clients(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_sdk_install_", dir=str(_tmp_parent())) as tmp:
            destdir = Path(tmp) / "destdir"
            package_out = Path(tmp) / "out"
            try:
                self._run_installed_sdk_example_smoke(app_dir, destdir, package_out)
            finally:
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )

            smoke_dir = package_out / "installed-sdk-example-smoke"
            self.assertTrue((smoke_dir / "mem_service_serving_example").exists())
            self.assertTrue((smoke_dir / "mem_service_pretraining_example").exists())

    @unittest.skipUnless(shutil.which("pkg-config"), "pkg-config is required")
    def test_installed_sdk_pkgconfig_smoke_builds_external_clients(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_sdk_pc_", dir=str(_tmp_parent())) as tmp:
            destdir = Path(tmp) / "destdir"
            package_out = Path(tmp) / "out"
            try:
                self._run_installed_sdk_pkgconfig_smoke(app_dir, destdir, package_out)
            finally:
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )

            smoke_dir = package_out / "installed-sdk-pkgconfig-smoke"
            self.assertTrue((smoke_dir / "mem_service_serving_example").exists())
            self.assertTrue((smoke_dir / "mem_service_pretraining_example").exists())

    def test_installed_sdk_runtime_smoke_runs_external_clients(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_sdk_runtime_", dir=str(_tmp_parent())) as tmp:
            destdir = Path(tmp) / "destdir"
            package_out = Path(tmp) / "out"
            try:
                result = self._run_installed_sdk_runtime_smoke(
                    app_dir,
                    destdir,
                    package_out,
                )
            finally:
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )

            self.assertIn("mem_service_serving_example=ok", result.stdout)
            self.assertIn("mem_service_pretraining_example=ok", result.stdout)
            self.assertIn("runtime_handoff_count=1", result.stdout)
            self.assertIn("execution_artifact_count=1", result.stdout)
            self.assertIn("training_artifact_count=6", result.stdout)

    @unittest.skipUnless(shutil.which("tar"), "tar is required")
    def test_make_package_tarball_smoke_creates_extractable_release_artifact(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_package_", dir=str(_tmp_parent())) as tmp:
            package_out = Path(tmp) / "package"
            try:
                subprocess.run(
                    [
                        "make",
                        "-C",
                        str(app_dir),
                        "CC=cc",
                        "CFLAGS=-O2 -Wall -Wextra",
                        "HOST_CC=cc",
                        f"PACKAGE_OUT_DIR={package_out}",
                        "package-tarball-smoke",
                    ],
                    cwd=REPO_ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                )
            finally:
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )

            tarball = package_out / "linqu_mem_service-installed-layout-v1.tar"
            listing = package_out / "linqu_mem_service-installed-layout-v1.tar.list"
            verify_root = package_out / "linqu_mem_service.installed-layout-v1.verify"
            release_manifest = (
                verify_root
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "release-manifest.txt"
            )
            package_manifest = (
                verify_root
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "package-manifest.txt"
            )
            pkgconfig = verify_root / "usr" / "lib" / "pkgconfig" / "lingqu-mem-service.pc"

            self.assertTrue(tarball.exists())
            self.assertIn("usr/bin/linqu_mem_service", listing.read_text())
            self.assertIn(
                "usr/libexec/lingqu/mem_service/linqu_mem_service_host",
                listing.read_text(),
            )
            self.assertIn(
                "usr/share/lingqu/mem_service/package-manifest.txt",
                listing.read_text(),
            )
            self.assertIn("usr/lib/pkgconfig/lingqu-mem-service.pc", listing.read_text())
            self.assertTrue(release_manifest.exists())
            self.assertTrue(package_manifest.exists())
            self.assertTrue(pkgconfig.exists())
            self.assertIn("distributable_package_format=tar", release_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", release_manifest.read_text())
            self.assertIn(
                "distributable_package_gate=package-tarball-smoke",
                release_manifest.read_text(),
            )
            self.assertIn("artifact_format=tar", package_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", package_manifest.read_text())
            self.assertIn(
                "sdk_sources=${sourcedir}/mem_service_client.c ${sourcedir}/mem_service_wire_client.c",
                pkgconfig.read_text(),
            )
            self.assertIn(
                "artifact_gate=package-tarball-smoke",
                package_manifest.read_text(),
            )

    @unittest.skipUnless(
        shutil.which("aarch64-linux-gnu-gcc")
        and shutil.which("ar")
        and shutil.which("gzip")
        and shutil.which("file"),
        "aarch64-linux-gnu-gcc, ar, gzip, and file are required",
    )
    def test_make_package_deb_smoke_creates_arm64_native_package(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_deb_", dir=str(_tmp_parent())) as tmp:
            package_out = Path(tmp) / "package"
            try:
                subprocess.run(
                    [
                        "make",
                        "-C",
                        str(app_dir),
                        "CFLAGS=-O2 -Wall -Wextra",
                        f"PACKAGE_OUT_DIR={package_out}",
                        "package-deb-smoke",
                    ],
                    cwd=REPO_ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                )
            finally:
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )

            deb = package_out / "linqu-mem-service_0.1.0-1_arm64.deb"
            listing = package_out / "linqu-mem-service_0.1.0-1_arm64.deb.list"
            verify_root = package_out / "linqu_mem_service.deb.verify"
            control = verify_root / "control" / "control"
            data_root = verify_root / "data"
            release_manifest = (
                data_root
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "release-manifest.txt"
            )
            package_manifest = (
                data_root
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "package-manifest.txt"
            )
            pkgconfig = data_root / "usr" / "lib" / "pkgconfig" / "lingqu-mem-service.pc"

            self.assertTrue(deb.exists())
            self.assertIn("debian-binary", listing.read_text())
            self.assertIn("control.tar.gz", listing.read_text())
            self.assertIn("data.tar.gz", listing.read_text())
            self.assertIn("Package: linqu-mem-service", control.read_text())
            self.assertIn("Architecture: arm64", control.read_text())
            self.assertTrue((data_root / "usr" / "bin" / "linqu_mem_service").exists())
            self.assertTrue(pkgconfig.exists())
            self.assertTrue(
                (
                    data_root
                    / "usr"
                    / "libexec"
                    / "lingqu"
                    / "mem_service"
                    / "linqu_mem_service_host"
                ).exists()
            )
            self.assertIn("native_package_format=deb", release_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", release_manifest.read_text())
            self.assertIn(
                "native_package_gate=package-deb-smoke",
                release_manifest.read_text(),
            )
            self.assertIn("native_package_format=deb", package_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", package_manifest.read_text())
            self.assertIn("prefix=/usr", pkgconfig.read_text())
            self.assertIn(
                "native_package_gate=package-deb-smoke",
                package_manifest.read_text(),
            )

    def test_make_package_rpm_smoke_is_toolchain_gated(self):
        app_dir = ROOT / "apps" / "mem_service"
        has_rpm_toolchain = (
            shutil.which("rpmbuild") and shutil.which("rpm2cpio") and shutil.which("cpio")
        )
        with tempfile.TemporaryDirectory(prefix="msvc_rpm_", dir=str(_tmp_parent())) as tmp:
            package_out = Path(tmp) / "package"
            result = subprocess.run(
                [
                    "make",
                    "-C",
                    str(app_dir),
                    "CFLAGS=-O2 -Wall -Wextra",
                    f"PACKAGE_OUT_DIR={package_out}",
                    "package-rpm-smoke",
                ],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                ["make", "-C", str(app_dir), "clean"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )

            rpm = package_out / "linqu-mem-service-0.1.0-1.aarch64.rpm"
            if not has_rpm_toolchain:
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(rpm.exists())
                self.assertIn("rpmbuild", result.stdout + result.stderr)
                return

            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            verify_root = package_out / "linqu_mem_service.rpm.verify"
            release_manifest = (
                verify_root
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "release-manifest.txt"
            )
            package_manifest = (
                verify_root
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "package-manifest.txt"
            )
            pkgconfig = verify_root / "usr" / "lib" / "pkgconfig" / "lingqu-mem-service.pc"
            self.assertTrue(rpm.exists())
            self.assertTrue((verify_root / "usr" / "bin" / "linqu_mem_service").exists())
            self.assertTrue(pkgconfig.exists())
            self.assertIn("rpm_native_package_format=rpm", release_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", release_manifest.read_text())
            self.assertIn(
                "rpm_native_package_gate=package-rpm-smoke",
                release_manifest.read_text(),
            )
            self.assertIn("rpm_package_format=rpm", package_manifest.read_text())
            self.assertIn("pkgconfig=lib/pkgconfig/lingqu-mem-service.pc", package_manifest.read_text())
            self.assertIn("prefix=/usr", pkgconfig.read_text())
            self.assertIn(
                "rpm_package_gate=package-rpm-smoke",
                package_manifest.read_text(),
            )

    def test_linux_ops_certification_smoke_is_external_gate(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_linux_ops_", dir=str(_tmp_parent())) as tmp:
            package_out = Path(tmp) / "package"
            evidence = package_out / "ops-certification-linux-ci.evidence"
            result = subprocess.run(
                [
                    "make",
                    "-C",
                    str(app_dir),
                    "CFLAGS=-O2 -Wall -Wextra",
                    f"PACKAGE_OUT_DIR={package_out}",
                    "linux-ops-certification-smoke",
                ],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                ["make", "-C", str(app_dir), "clean"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(evidence.exists())
            self.assertTrue(
                "rpmbuild" in result.stdout + result.stderr
                or "ops-certification-linux-ci-smoke: fail-closed" in result.stderr
                or "ops-certification-upgrade-rollback.marker" in result.stdout
                + result.stderr
            )

        with tempfile.TemporaryDirectory(prefix="msvc_linux_deploy_", dir=str(_tmp_parent())) as tmp:
            package_out = Path(tmp) / "package"
            evidence = package_out / "ops-certification-linux-ci.evidence"
            marker = package_out / "ops-certification-upgrade-rollback.marker"
            upgrade = subprocess.run(
                [
                    "make",
                    "-C",
                    str(app_dir),
                    "CFLAGS=-O2 -Wall -Wextra",
                    f"PACKAGE_OUT_DIR={package_out}",
                    "linux-ops-upgrade-rollback-smoke",
                ],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            result = subprocess.run(
                [
                    "make",
                    "-C",
                    str(app_dir),
                    "CFLAGS=-O2 -Wall -Wextra",
                    f"PACKAGE_OUT_DIR={package_out}",
                    "linux-ops-deployment-smoke",
                ],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                ["make", "-C", str(app_dir), "clean"],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertNotEqual(upgrade.returncode, 0)
            self.assertFalse(marker.exists())
            self.assertTrue(
                "rpmbuild" in upgrade.stdout + upgrade.stderr
                or "uname -s" in upgrade.stdout + upgrade.stderr
                or "id -u" in upgrade.stdout + upgrade.stderr
                or "/run/systemd/system" in upgrade.stdout + upgrade.stderr
                or 'test -n ""' in upgrade.stdout + upgrade.stderr
                or "OPS_CERTIFICATION_ROLLBACK_RPM" in upgrade.stdout + upgrade.stderr
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(evidence.exists())
            self.assertTrue(
                "rpmbuild" in result.stdout + result.stderr
                or "uname -s" in result.stdout + result.stderr
                or "id -u" in result.stdout + result.stderr
                or "/run/systemd/system" in result.stdout + result.stderr
                or "ops-certification-linux-ci-smoke: fail-closed" in result.stderr
            )

    def test_installed_host_service_manager_and_collector_smoke(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_host_service_", dir=str(_tmp_parent())) as tmp:
            destdir = Path(tmp)
            socket_path = _tmp_parent() / f"linqu_mem_service_host_{os.getpid()}_{id(self)}.sock"
            metrics_port = self._free_tcp_port()
            config_dir = destdir / "etc" / "lingqu" / "mem_service"
            state_dir = destdir / "var" / "lib" / "lingqu" / "mem_service"
            config_path = config_dir / "mem_service.conf"
            store_path = state_dir / "service.store"
            service_unit = (
                destdir
                / "usr"
                / "share"
                / "lingqu"
                / "mem_service"
                / "deploy"
                / "linqu_mem_service.host.service"
            )
            installed_service_unit = (
                destdir / "usr" / "lib" / "systemd" / "system" /
                "linqu_mem_service.host.service"
            )
            host_binary = (
                destdir
                / "usr"
                / "libexec"
                / "lingqu"
                / "mem_service"
                / "linqu_mem_service_host"
            )
            proc = None

            try:
                self._install_release_layout(app_dir, destdir)
                self.assertTrue(installed_service_unit.exists())
                self.assertIn(
                    "RuntimeDirectory=lingqu",
                    installed_service_unit.read_text(),
                )
                self.assertIn(
                    "StateDirectory=lingqu/mem_service_host",
                    installed_service_unit.read_text(),
                )
                self.assertEqual(
                    self._parse_exec_start(installed_service_unit),
                    self._parse_exec_start(service_unit),
                )
                config_dir.mkdir(parents=True, exist_ok=True)
                state_dir.mkdir(parents=True)
                config_path.write_text(
                    f"listen=unix:{socket_path}\n"
                    f"store={store_path}\n"
                    "backend=snapshot+journal\n"
                    "auth_mode=none\n"
                    "metrics_mode=text-kv\n"
                    f"metrics_listen=tcp:127.0.0.1:{metrics_port}\n"
                    "adapter_enablement=core\n"
                )

                exec_start = self._parse_exec_start(service_unit)
                self.assertEqual(
                    exec_start[0],
                    "/usr/libexec/lingqu/mem_service/linqu_mem_service_host",
                )
                self.assertEqual(exec_start[1:3], ["serve", "--config"])
                self.assertEqual(
                    exec_start[3],
                    "/etc/lingqu/mem_service/mem_service.host.conf",
                )
                command = [str(host_binary), *exec_start[1:3], str(config_path)]
                proc = subprocess.Popen(
                    command,
                    cwd=REPO_ROOT,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                self._wait_installed_service_ready(proc, host_binary, socket_path)

                put = subprocess.run(
                    [
                        str(host_binary),
                        "put-object",
                        "--connect",
                        f"unix:{socket_path}",
                        "--key",
                        "collector-smoke-object",
                        "--version",
                        "1",
                        "--checksum",
                        "8001",
                    ],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(put.returncode, 0, put.stderr + put.stdout)

                scraped = self._http_metrics_request(metrics_port)
                collected = self._collect_prometheus_metrics(scraped)
                self.assertGreaterEqual(
                    collected["lingqu_mem_service_request_count"],
                    2,
                )
                self.assertGreaterEqual(
                    collected["lingqu_mem_service_health_count"],
                    1,
                )
                self.assertEqual(
                    collected["lingqu_mem_service_put_object_count"],
                    1,
                )

                proc.terminate()
                stdout, stderr = proc.communicate(timeout=5)
                rc = proc.returncode
                proc = None
                self.assertEqual(rc, 0, stderr + stdout)
                self.assertIn("status=ready", stdout)
                self.assertIn(f"listen=unix:{socket_path}", stdout)
                self.assertIn(f"store={store_path}", stdout)
                self.assertIn(f"metrics_listen=tcp:127.0.0.1:{metrics_port}", stdout)
                self.assertIn("status=stopped", stdout)
                self.assertFalse(socket_path.exists(), "service socket should be removed on shutdown")
            finally:
                if proc is not None and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.communicate(timeout=5)
                socket_path.unlink(missing_ok=True)
                subprocess.run(
                    ["make", "-C", str(app_dir), "clean"],
                    cwd=REPO_ROOT,
                    check=False,
                    capture_output=True,
                    text=True,
                )


if __name__ == "__main__":
    unittest.main()
