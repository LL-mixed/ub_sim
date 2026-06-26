import os
import shutil
import socket
import subprocess
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
SDK_EXAMPLES_DIR = ROOT / "apps" / "mem_service" / "examples"


def _tmp_parent() -> Path:
    private_tmp = Path("/private/tmp")
    if private_tmp.exists():
        return private_tmp
    return Path(tempfile.gettempdir())


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
                "    struct mem_service_wire_client_options options = {\n"
                "        .timeout_ms = 25,\n"
                "    };\n"
                "    const char *status = mem_service_wire_status_name("
                "MEM_SERVICE_WIRE_STATUS_OK);\n"
                "    const char *spec = mem_service_default_unix_socket_spec();\n"
                "    mem_service_client_init_with_options(&client, spec, &options);\n"
                "    return status != 0 && client.connect_spec == spec && "
                "client.wire_options.timeout_ms == 25 ? 0 : 1;\n"
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
        )

    def _run_client(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.binary), *args],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
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

    def _start_server(self) -> subprocess.Popen:
        proc = subprocess.Popen(
            [
                str(self.binary),
                "serve",
                "--listen",
                f"unix:{self.socket}",
                "--store",
                str(self.store),
            ],
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
                self.fail(
                    f"mem_service daemon exited rc={proc.returncode}\nstdout={stdout}\nstderr={stderr}"
                )
            health = self._run_client("health", "--connect", f"unix:{self.socket}")
            if health.returncode == 0 and "status=ok" in health.stdout:
                return proc
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service daemon did not become ready")

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
        self.assertIn("operations=23", fixtures.stdout)
        self.assertIn("schema_manifest_len=8695", fixtures.stdout)
        self.assertIn("schema_manifest_checksum=0x8a8ca3c4", fixtures.stdout)

        manifest = self._run_client("release-manifest")
        expected = (ROOT / "apps" / "mem_service" / "release-manifest.txt").read_text()
        self.assertEqual(manifest.returncode, 0, manifest.stderr + manifest.stdout)
        self.assertEqual(manifest.stdout, expected)

    def test_wire_schema_cli_matches_checked_in_contract(self):
        fixtures = self._run_client("wire-schema-fixtures")
        self.assertEqual(fixtures.returncode, 0, fixtures.stderr + fixtures.stdout)
        self.assertIn("status=ok", fixtures.stdout)
        self.assertIn("manifest_len=8695", fixtures.stdout)
        self.assertIn("manifest_checksum=0x8a8ca3c4", fixtures.stdout)
        self.assertIn("operations=23", fixtures.stdout)
        self.assertIn("fields=102", fixtures.stdout)

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
    def test_make_install_smoke_creates_release_layout(self):
        app_dir = ROOT / "apps" / "mem_service"
        with tempfile.TemporaryDirectory(prefix="msvc_install_", dir=str(_tmp_parent())) as tmp:
            destdir = Path(tmp)
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
            try:
                subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)
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
            wire_schema = destdir / "usr" / "share" / "lingqu" / "mem_service" / "wire-schema.txt"
            self.assertTrue(manifest.exists())
            self.assertTrue(wire_schema.exists())
            self.assertIn("core_binary=bin/linqu_mem_service", manifest.read_text())
            self.assertIn("wire_schema_manifest_checksum=0x8a8ca3c4", manifest.read_text())
            self.assertIn("mem_service_serving_example.c", manifest.read_text())
            self.assertIn("mem_service_pretraining_example.c", manifest.read_text())
            self.assertEqual(wire_schema.read_text(), WIRE_SCHEMA_MANIFEST.read_text())


if __name__ == "__main__":
    unittest.main()
