#!/usr/bin/env python3
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class Qwen3DenseEnvTest(unittest.TestCase):
    def run_env_probe(self, config, profile="qwen3_dense"):
        common = Path(__file__).resolve().parents[1] / "scripts" / "qemu_ub_common.sh"
        with tempfile.TemporaryDirectory() as tmp:
            model_dir = Path(tmp)
            (model_dir / "config.json").write_text(json.dumps(config), encoding="utf-8")
            (model_dir / "tokenizer.json").write_text("{}", encoding="utf-8")
            (model_dir / "model.safetensors.index.json").write_text("{}", encoding="utf-8")

            probe = (
                "source \"$1\"\n"
                "SIM_UAPI_W4_CHIPBACKEND_PROFILE=\"$3\"\n"
                "SIM_QWEN3_DENSE_WEIGHTS_PATH=\"$2\"\n"
                "qwen3_dense_apply_config_env\n"
                "printf '%s\\n' \"$SIM_UAPI_W4_CHIPBACKEND_PROFILE\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_MODEL_KEY\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_KV_STATE_BYTES\"\n"
            )
            result = subprocess.run(
                ["zsh", "-c", probe, "zsh", str(common), str(model_dir), profile],
                check=True,
                capture_output=True,
                text=True,
            )
            return result.stdout.strip().splitlines()

    def test_14b_config_uses_generic_profile_and_exports_dimensions(self):
        values = self.run_env_probe(
            {
                "_name_or_path": "Qwen/Qwen3-14B",
                "vocab_size": 151936,
                "hidden_size": 5120,
                "intermediate_size": 17408,
                "num_hidden_layers": 40,
                "num_attention_heads": 40,
                "num_key_value_heads": 8,
                "head_dim": 128,
                "max_position_embeddings": 40960,
                "rope_theta": 1000000,
            }
        )

        self.assertEqual(values, ["qwen3_dense", "qwen3-14b", "40", "1310720", "10240", "327680"])

    def test_reference_config_uses_generic_profile_by_default(self):
        values = self.run_env_probe(
            {
                "_name_or_path": "Qwen/Qwen3-0.6B",
                "vocab_size": 151936,
                "hidden_size": 1024,
                "intermediate_size": 3072,
                "num_hidden_layers": 28,
                "num_attention_heads": 16,
                "num_key_value_heads": 8,
                "head_dim": 128,
                "max_position_embeddings": 40960,
                "rope_theta": 1000000,
            }
        )

        self.assertEqual(values, ["qwen3_dense", "qwen3-0-6b", "28", "262144", "2048", "229376"])

    def test_reference_profile_remains_explicit_legacy_alias_for_0_6b(self):
        values = self.run_env_probe(
            {
                "_name_or_path": "Qwen/Qwen3-0.6B",
                "vocab_size": 151936,
                "hidden_size": 1024,
                "intermediate_size": 3072,
                "num_hidden_layers": 28,
                "num_attention_heads": 16,
                "num_key_value_heads": 8,
                "head_dim": 128,
                "max_position_embeddings": 40960,
                "rope_theta": 1000000,
            },
            profile="qwen3_dense_reference",
        )

        self.assertEqual(
            values, ["qwen3_dense_reference", "qwen3-0-6b", "28", "262144", "2048", "229376"]
        )

    def test_qwen3_dense_two_step_wrapper_has_stable_defaults(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        wrapper = script_dir / "run_ub_eight_node_w4_guest_qwen3_dense_2step.sh"

        self.assertTrue(wrapper.exists())
        self.assertTrue(wrapper.stat().st_mode & 0o111)

        text = wrapper.read_text(encoding="utf-8")
        self.assertIn("SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}", text)
        self.assertIn("SIM_QWEN3_GUEST_DECODE_STEPS:-2", text)
        self.assertIn("SIM_QWEN3_DENSE_WEIGHTS_PATH:-", text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"', text)

    def test_eight_node_runner_passes_decode_round_barrier_timeout(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        runner = script_dir / "run_ub_eight_node_w4_guest.sh"
        launcher = script_dir / "launch_ub_eight_node_headless.sh"

        runner_text = runner.read_text(encoding="utf-8")
        launcher_text = launcher.read_text(encoding="utf-8")

        self.assertIn("SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}", runner_text)
        self.assertIn("SIM_UAPI_W5_PROFILE", runner_text)
        self.assertIn("w5_profile_default_w4_backend", runner_text)
        self.assertIn("validate_w5_profile_runtime", runner_text)
        self.assertIn("SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS", runner_text)
        self.assertIn("DEMO_WAIT_SECS * 1000", runner_text)
        self.assertIn("SIM_QWEN3_RUNTIME_RANGE_WAIT_MS", runner_text)
        self.assertIn(
            "DEMO_WAIT_SECS * SIM_QWEN3_GUEST_DECODE_STEPS * 1000",
            runner_text,
        )
        self.assertIn("SIM_UAPI_W5_PROFILE", launcher_text)
        self.assertIn("SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS", launcher_text)
        self.assertIn("SIM_QWEN3_RUNTIME_RANGE_WAIT_MS", launcher_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_STATE_REF", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST", runner_text)
        self.assertIn(
            'SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST"',
            runner_text,
        )
        self.assertIn(
            'SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT" \\',
            runner_text,
        )
        self.assertIn("qwen3_engram_context_refs_configured", runner_text)
        self.assertIn("qwen3_engram_context_op_enabled", runner_text)
        self.assertIn("validate_qwen3_engram_context_refs", runner_text)
        self.assertIn("context op requires EngramStateObjectRef", runner_text)
        self.assertIn("component refs are not a real W5 entrypoint", runner_text)
        self.assertIn("qwen3_engram_state_object_ref", runner_text)
        self.assertIn("validate_w5_engram_context_summary", runner_text)
        self.assertIn("target=uapi_object_ref", runner_text)
        self.assertIn("modes=[^ ]*object-ref", runner_text)
        self.assertIn("validate_w5_boundary_observation_summary", runner_text)
        self.assertIn("memory_boundary_observation_summary", runner_text)
        self.assertIn("w5_shortpath_execute_enabled", runner_text)
        self.assertIn("assert_log_count", runner_text)
        self.assertIn(
            "qwen3_w5_memory_shortpath_commit:${SIM_QWEN3_GUEST_DECODE_STEPS}",
            runner_text,
        )
        self.assertIn(
            "qwen3_w5_memory_terminal_logits_selected:${SIM_QWEN3_GUEST_DECODE_STEPS}",
            runner_text,
        )
        self.assertIn("lookup_hits=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn(
            "idle_expected=$((SIM_QWEN3_GUEST_DECODE_STEPS * (${#NODE_IDS[@]} - 1)))",
            runner_text,
        )
        self.assertIn("worker_timing_records=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn("idle_timing_records=${idle_expected}", runner_text)
        self.assertIn("status=idle_no_work_item", runner_text)
        self.assertIn("obmm_pool: not_observed", runner_text)
        self.assertIn("active_worker_records=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn("idle_worker_records=${idle_expected}", runner_text)
        self.assertIn("obmm_pool: unavailable", runner_text)
        self.assertIn("fallback=runtime_forward_metadata", runner_text)
        self.assertIn("payload_bytes=[0-9]+,[0-9]+", runner_text)
        self.assertIn("actions=jump-to-terminal .*artifact_kinds=logits", runner_text)
        self.assertIn("shortpath_ids=none", runner_text)
        self.assertIn("support_ids=none", runner_text)
        self.assertIn("artifact_kinds=none", runner_text)
        self.assertIn(
            "W5 shortpath execution summary incomplete or unauditable",
            runner_text,
        )
        self.assertIn("W5 shortpath timing record counts are incomplete", runner_text)
        self.assertIn("W5 shortpath downstream timing is not idle-only", runner_text)
        self.assertIn("W5 shortpath downstream handoff is not idle-only", runner_text)
        self.assertIn("W5 shortpath pool usage summary is ambiguous", runner_text)
        self.assertIn("W5 shortpath summary contains stale fallback/missing/ambiguous markers", runner_text)
        self.assertIn("validate_w5_artifact_sizes", runner_text)
        self.assertIn("--validate-w5-artifact-sizes-only", runner_text)
        self.assertIn('TEE_BIN="${TEE_BIN:-/usr/bin/tee}"', runner_text)
        self.assertIn("zstat -H file_stat +size", runner_text)
        self.assertIn('$shortpath_kv_stream" == /tmp/*', runner_text)
        self.assertIn("SIM_W5_MAX_MEMORY_STORE_JSON_BYTES:-16777216", runner_text)
        self.assertIn("SIM_W5_MAX_OBJECT_STORE_JSON_BYTES:-8388608", runner_text)
        self.assertIn("SIM_W5_MAX_OBJECT_STORE_BIN_BYTES:-268435456", runner_text)
        self.assertIn("SIM_W5_MAX_SHORTPATH_STREAM_BYTES:-1048576", runner_text)
        self.assertIn("SIM_W5_MAX_SHORTPATH_KV_STREAM_BYTES:-1048576", runner_text)
        self.assertIn("W5 artifact size too large", runner_text)
        self.assertIn("W5 artifact size ok", runner_text)
        self.assertIn("W5 shortpath scheduler no-dispatch per step", runner_text)
        self.assertIn("W5 shortpath terminal commit observed per step", runner_text)
        self.assertIn("W5 shortpath idle timing per step", runner_text)
        self.assertIn("W5 shortpath downstream range forward", runner_text)
        self.assertIn("W5 shortpath downstream runtime output publish", runner_text)
        self.assertIn("W5 shortpath boundary commit per step", runner_text)
        self.assertIn("W5 shortpath terminal logits selected per step", runner_text)
        self.assertIn("W5 shortpath terminal token publish per step", runner_text)
        self.assertIn("idx > 1", runner_text)
        self.assertIn("observation_id=boundary-observation/${RUN_ID_BASE}", runner_text)
        self.assertIn("source=w5_guest_range_exit hidden_backend=obmm_shmem", runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_STATE_REF", launcher_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR", launcher_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT", launcher_text)
        self.assertIn("SIM_W5_RUN_ID", runner_text)
        self.assertIn("SIM_W5_RUN_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_LOOKUP_MODE", runner_text)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_LOOKUP_BACKEND", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_DECISION_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_START", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_END", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_KIND", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_REF", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_START", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_END", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_POSITION", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_EXECUTE", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_PLAN_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_IDS", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_REFS", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF", runner_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_STORE", launcher_text)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_LOOKUP_BACKEND", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_DECISION_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_START", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_END", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_KIND", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_REF", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_START", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_END", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_POSITION", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_EXECUTE", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_PLAN_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_IDS", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_REFS", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF", launcher_text)

    def test_w5_artifact_size_validation_cli_uses_host_registry_for_guest_tmp_streams(self):
        runner = Path(__file__).resolve().parents[1] / "scripts" / "run_ub_eight_node_w4_guest.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            registry_dir = tmp_path / "registry"
            registry_dir.mkdir()
            memory_store = tmp_path / "memory_store.json"
            object_store = tmp_path / "object_store.json"
            object_bin = tmp_path / "object_store.bin"
            shortpath_stream = registry_dir / "w5_memory_shortpath_stream.txt"
            shortpath_kv_stream = registry_dir / "w5_memory_shortpath_kv_stream.txt"

            for path in (memory_store, object_store, object_bin, shortpath_stream, shortpath_kv_stream):
                path.write_bytes(b"ok")

            env = os.environ.copy()
            env.update(
                {
                    "SIM_UAPI_W5_PROFILE": "qwen3_14b_engram_decode",
                    "SIM_W5_MEMORY_STORE": str(memory_store),
                    "SIM_W5_MEMORY_OBJECT_STORE": str(object_store),
                    "SIM_W5_MEMORY_REGISTRY_DIR": str(registry_dir),
                    "SIM_W5_MEMORY_SHORTPATH_STREAM_PATH": "/tmp/w5_memory_shortpath_stream.txt",
                    "SIM_W5_MEMORY_SHORTPATH_KV_STREAM_PATH": "/tmp/w5_memory_shortpath_kv_stream.txt",
                    "TRACE_FILE": str(tmp_path / "trace.txt"),
                }
            )

            result = subprocess.run(
                ["zsh", str(runner), "--validate-w5-artifact-sizes-only"],
                check=True,
                capture_output=True,
                text=True,
                env=env,
            )

            self.assertIn(f"label=shortpath_stream bytes=2", result.stderr)
            self.assertIn(str(shortpath_stream), result.stderr)
            self.assertIn(f"label=shortpath_kv_stream bytes=2", result.stderr)
            self.assertIn(str(shortpath_kv_stream), result.stderr)
            self.assertNotIn("/tmp/w5_memory_shortpath_kv_stream.txt", result.stderr)

    def test_w5_artifact_size_validation_cli_fails_on_oversized_artifact(self):
        runner = Path(__file__).resolve().parents[1] / "scripts" / "run_ub_eight_node_w4_guest.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            registry_dir = tmp_path / "registry"
            registry_dir.mkdir()
            memory_store = tmp_path / "memory_store.json"
            object_store = tmp_path / "object_store.json"
            object_bin = tmp_path / "object_store.bin"
            shortpath_stream = registry_dir / "w5_memory_shortpath_stream.txt"
            shortpath_kv_stream = registry_dir / "w5_memory_shortpath_kv_stream.txt"

            for path in (memory_store, object_store, object_bin, shortpath_stream):
                path.write_bytes(b"ok")
            shortpath_kv_stream.write_bytes(b"too-large")

            env = os.environ.copy()
            env.update(
                {
                    "SIM_UAPI_W5_PROFILE": "qwen3_14b_engram_decode",
                    "SIM_W5_MEMORY_STORE": str(memory_store),
                    "SIM_W5_MEMORY_OBJECT_STORE": str(object_store),
                    "SIM_W5_MEMORY_REGISTRY_DIR": str(registry_dir),
                    "SIM_W5_MEMORY_SHORTPATH_KV_STREAM_PATH": "/tmp/w5_memory_shortpath_kv_stream.txt",
                    "SIM_W5_MAX_SHORTPATH_KV_STREAM_BYTES": "1",
                    "TRACE_FILE": str(tmp_path / "trace.txt"),
                }
            )

            result = subprocess.run(
                ["zsh", str(runner), "--validate-w5-artifact-sizes-only"],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("FAIL: W5 artifact size too large label=shortpath_kv_stream", result.stderr)
            self.assertIn("bytes=9 max_bytes=1", result.stderr)

    def test_guest_consumes_w5_prefix_cache_reuse_as_kv_object_ref(self):
        guest_source = (
            Path(__file__).resolve().parents[1] / "w4_guest_qemu_demo.c"
        ).read_text(encoding="utf-8")
        db_service_source = (
            Path(__file__).resolve().parents[1] / "w4_kvcache_db_service.c"
        ).read_text(encoding="utf-8")
        db_service_header = (
            Path(__file__).resolve().parents[1] / "w4_kvcache_db_service.h"
        ).read_text(encoding="utf-8")
        cli_source = (
            Path(__file__).resolve().parents[3] / "crates" / "sim-cli" / "src" / "main.rs"
        ).read_text(encoding="utf-8")

        self.assertIn("qwen3_memory_prefix_cache_kv_ref", guest_source)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF", guest_source)
        self.assertIn("W4_QWEN3_OBMM_KIND_QWEN3_KV_STATE", guest_source)
        self.assertIn("qwen3_w5_memory_prefix_cache_kv_loaded", guest_source)
        self.assertIn("source=lingqu_memory_service target=uapi_object_ref", guest_source)
        self.assertIn("jump-to-terminal", guest_source)
        self.assertIn("artifact_kind=%s", guest_source)
        self.assertIn("jump-to-terminal contract invalid", guest_source)
        self.assertIn("qwen3_memory_shortpath_terminal_logits_record", guest_source)
        self.assertIn("qwen3_read_object_service_payload", guest_source)
        self.assertIn("W4_QWEN3_OBJECT_SERVICE_PAYLOAD_INDEX_MAGIC", guest_source)
        self.assertIn("w4_db_obmm_service_v0_ensure_cluster_runtime", guest_source)
        self.assertIn("obmm_cluster_runtime_bootstrap", db_service_source)
        self.assertIn("w4_db_cluster_runtime_require", db_service_source)
        self.assertIn("lazy_activation_forbidden", db_service_source)
        self.assertIn("peer_not_bootstrapped", db_service_source)
        self.assertIn("after=obmm_cluster_runtime_bootstrap", guest_source)
        self.assertIn(
            "needs_engram_history =\n                    local_decode_node == qwen3_engram_config.owner_node",
            guest_source,
        )
        self.assertNotIn("local_decode_node == 0U ||", guest_source)
        self.assertNotIn("local_decode_node + 1U == cluster_node_count ||", guest_source)
        self.assertIn("w4_db_take_pending_qwen3_object_desc", db_service_source)
        self.assertIn("w4_db_take_pending_qwen3_object_kind_len_desc", db_service_source)
        self.assertIn("qwen3_w5_memory_terminal_logits_loaded", guest_source)
        self.assertIn(
            "w4_db_obmm_service_v0_publish_shortpath_terminal_token_result",
            guest_source,
        )
        self.assertIn("entry->target_node == local_node + 1U", guest_source)
        self.assertIn("runtime_kv_checksum = w4_qwen3_hidden_payload_checksum", guest_source)
        self.assertIn("runtime_checksum=0x%016", guest_source)
        self.assertIn("qwen3_w5_memory_terminal_logits_selected", guest_source)
        self.assertNotIn("record->sampled_token == record->runner_up_token", guest_source)
        self.assertNotIn("engram_policy_requires_materialized_owner", guest_source)
        self.assertIn("source=shortpath_boundary_policy", guest_source)
        self.assertIn("target=terminal_token_result", guest_source)
        self.assertIn("publish_hidden=0", guest_source)
        self.assertIn("qwen3_memory_shortpath_audit_id", guest_source)
        self.assertIn("qwen3_memory_shortpath_support_audit_id", guest_source)
        self.assertIn("qwen3_memory_shortpath_artifact_kind_audit", guest_source)
        self.assertIn("shortpath_catalog_entries=%", guest_source)
        self.assertIn("runtime_service_catalog", guest_source)
        self.assertIn("boundary_registry", guest_source)
        self.assertIn("qwen3_memory_shortpath_downstream_kv_support_complete", guest_source)
        self.assertIn("skipped_downstream_kv_state_unavailable", guest_source)
        self.assertIn("shortpath_execution_guard", guest_source)
        self.assertIn("qwen3_round_decode_position", guest_source)
        self.assertIn(
            "qwen3_prompt_base_token_count + guest_decode_step",
            guest_source,
        )
        self.assertIn("qwen3_decode_position_resolved", guest_source)
        self.assertIn(
            "qwen3_w5_memory_service_lookup_boundary(\n"
            "        memory_config,\n"
            "        dispatch_node,\n"
            "        cluster_node_count,\n"
            "        layer_start,\n"
            "        layer_end,\n"
            "        decode_step,\n"
            "        position,",
            guest_source,
        )
        self.assertIn("w4_db_obmm_service_v0_publish_runtime_range_kv_state", guest_source)
        self.assertIn("qwen3_decode_round_scheduler_no_dispatch", guest_source)
        self.assertIn("work_item=none", guest_source)
        self.assertIn("dispatch=skipped status=no_dispatch", guest_source)
        self.assertIn("qwen3_work_item_scheduler_wait", guest_source)
        self.assertIn("qwen3_work_item_scheduler_dispatch", guest_source)
        self.assertIn("w4_db_obmm_service_v0_wait_scheduler_work_item", guest_source)
        self.assertIn("struct w4_db_scheduler_work_item", db_service_header)
        self.assertIn("W4_DB_SCHEDULER_WORK_ITEM_RANGE_FORWARD", db_service_header)
        self.assertIn("W4_DB_SCHEDULER_WORK_ITEM_NO_DISPATCH", db_service_header)
        self.assertIn("qwen3_memory_service_boundary_lookup_request", guest_source)
        self.assertIn("qwen3_memory_service_boundary_lookup_response", guest_source)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_REGISTRY_REF", guest_source)
        self.assertIn("qwen3_read_w5_boundary_registry_object", guest_source)
        self.assertIn("qwen3_w5_memory_boundary_registry_loaded", guest_source)
        self.assertIn("artifact_ref=%s", guest_source)
        self.assertIn("runtime_service_catalog", guest_source)
        self.assertIn("registry_index=%", guest_source)
        self.assertIn("source=boundary_controller target=lingqu_memory_service", guest_source)
        self.assertIn("source=lingqu_memory_service target=boundary_controller", guest_source)
        self.assertIn("qwen3_boundary_controller_lookup", guest_source)
        self.assertIn("qwen3_boundary_controller_input", guest_source)
        self.assertIn("qwen3_boundary_controller_downstream_work_item", guest_source)
        self.assertIn("source=range_worker target=boundary_controller", guest_source)
        self.assertIn("source=boundary_controller target=lingqu_memory_service", guest_source)
        self.assertIn("source=boundary_controller target=work_queue", guest_source)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_LOOKUP_MODE", guest_source)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_LOOKUP_BACKEND", guest_source)
        self.assertIn("boundary_lookup_backend", guest_source)
        self.assertIn("shortpath_lookup_mode", guest_source)
        self.assertIn("staged_registry", guest_source)
        self.assertIn("runtime_service", guest_source)
        self.assertNotIn('"online_registry"', guest_source)
        self.assertIn("mode=%s backend=%s status=hit", guest_source)
        self.assertIn("mode=%s backend=%s status=miss", guest_source)
        self.assertIn("qwen3_no_work_item_service_coverage:", guest_source)
        self.assertIn("qwen3_pre_resolved_range_input", guest_source)
        self.assertIn("qwen3_decode_round_terminal_committed", db_service_source)
        self.assertIn(
            "allow_terminal_commit",
            db_service_source,
        )
        self.assertIn(
            "for (node_idx = 0; node_idx < cluster_node_count; ++node_idx)",
            db_service_source,
        )
        self.assertIn("broadcast_targets=%u", db_service_source)
        self.assertIn("w4_db_take_pending_qwen3_token_result_desc", db_service_source)
        self.assertIn("receive=descriptor", db_service_source)
        self.assertIn("target=decode_round_scheduler receive=descriptor", db_service_source)
        self.assertIn(
            "false,\n        view_out);",
            db_service_source,
        )
        self.assertNotIn("qwen3_step_work_item_absent", guest_source)
        self.assertNotIn("qwen3_step_work_item_terminal_observed", db_service_source)
        self.assertNotIn(
            "terminal_round_committed_input",
            guest_source,
        )
        self.assertIn("qwen3_w5_memory_shortpath_kv_lazy_resolve", guest_source)
        self.assertNotIn("qwen3_range_kv_state_lazy_fallback", guest_source)
        self.assertNotIn("reason=intermediate_step_kv_absent", guest_source)
        self.assertIn("mode=exact_previous_step", guest_source)
        self.assertIn("shortpath exact kv materialize failed", guest_source)
        self.assertIn("reason=not_lazy_work_item_resolve", guest_source)
        self.assertIn("shortpath_worker_stateless", guest_source)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR", cli_source)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT", cli_source)
        self.assertIn("w5_kv_hot_object_ref_from_object_service", cli_source)
        self.assertIn("qwen3_object_registry_path_in_dir", cli_source)
        self.assertIn("trigger=work_item_lazy_resolve scope=local_range", guest_source)
        self.assertIn(
            '" node=%u consumer_step=%" PRIu64 " kv_step=%" PRIu64',
            guest_source,
        )
        self.assertIn("w4_db_obmm_service_v0_try_resolve_range_kv_state_view", guest_source)
        self.assertNotIn("while (candidate > 0U)", guest_source)
        self.assertIn("qwen3_decode_round_idle_timing", guest_source)
        self.assertIn('engram_range_work_item ? "ok" : "idle"', guest_source)
        self.assertIn('engram_range_work_item ? "range_or_shortpath" : "none"', guest_source)
        self.assertIn("engram_range_work_item ? input_wait_ms : 0ULL", guest_source)
        self.assertIn("terminal logits candidate_count invalid", guest_source)
        self.assertIn("terminal logits sampled candidate metadata invalid", guest_source)
        self.assertNotIn("record->candidate_count = 1;", guest_source)
        self.assertNotIn("record->candidate_tokens[0] = record->sampled_token", guest_source)
        self.assertNotIn("qwen3_w5_memory_shortpath_downstream_skip", guest_source)
        self.assertNotIn("qwen3_w5_memory_terminal_publish_skip", guest_source)
        self.assertNotIn("fallback=runtime_forward_metadata", guest_source)
        self.assertNotRegex(
            guest_source,
            r"uapi_qwen3_range_compute_contract[\s\S]{0,400}"
            r"source=runtime_forward output=metadata status=ok",
        )
        self.assertIn("shortpath_boundary", db_service_source)

    def test_w5_inference_cluster_runner_delegates_to_legacy_compatible_runner(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        runner = script_dir / "run_ub_eight_node_w5_inference_cluster.sh"
        generic = script_dir / "run_ub_w5_inference_cluster.sh"
        config_runner = script_dir / "run_w5_cluster_config.sh"
        summary = script_dir / "w5_inference_cluster_summary.py"

        self.assertTrue(runner.exists())
        self.assertTrue(runner.stat().st_mode & 0o111)
        self.assertTrue(generic.exists())
        self.assertTrue(generic.stat().st_mode & 0o111)
        self.assertTrue(config_runner.exists())
        self.assertTrue(config_runner.stat().st_mode & 0o111)
        self.assertTrue(summary.exists())
        self.assertTrue(summary.stat().st_mode & 0o111)

        runner_text = runner.read_text(encoding="utf-8")
        generic_text = generic.read_text(encoding="utf-8")
        config_runner_text = config_runner.read_text(encoding="utf-8")
        legacy_runner_text = (script_dir / "run_ub_eight_node_w4_guest.sh").read_text(encoding="utf-8")
        summary_text = summary.read_text(encoding="utf-8")

        self.assertIn("SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode", runner_text)
        self.assertIn("SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP", runner_text)
        self.assertIn("SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP", runner_text)
        self.assertIn("SIM_W5_MEMORY_OBSERVATION_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_REUSE_RUN_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_REUSE_OUT_DIR", runner_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_OBJECT_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_REGISTRY_REF", legacy_runner_text)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_REGISTRY_COUNT", legacy_runner_text)
        self.assertIn("SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_DECISION_IDS", runner_text)
        self.assertIn("SIM_W5_MEMORY_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_OBJECT_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_ENGRAM_STATE", runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_POOL", runner_text)
        self.assertIn("SIM_W5_MEMORY_REGISTRY_DIR", runner_text)
        self.assertIn("target/debug/sim-cli", runner_text)
        self.assertIn("cargo build -p sim-cli", runner_text)
        self.assertIn("unset SIM_CLI_BIN so the runner builds", runner_text)
        self.assertIn("--memory-runtime-boundary-lookup", runner_text)
        self.assertIn("--memory-online-boundary-lookup", runner_text)
        self.assertIn("--memory-observation-store", runner_text)
        self.assertIn("--memory-store", runner_text)
        self.assertIn("--memory-object-store", runner_text)
        self.assertIn("--memory-engram-state", runner_text)
        self.assertIn("--memory-registry-dir", runner_text)
        self.assertIn("--memory-decision-store", runner_text)
        self.assertIn("--memory-decision-object-store", runner_text)
        self.assertIn("--memory-boundary-observation-run-id", runner_text)
        self.assertIn("--memory-shortpath-decision-ids", runner_text)
        self.assertIn("--memory-store", runner_text)
        self.assertIn("--memory-object-store", runner_text)
        self.assertIn("--memory-engram-state", runner_text)
        self.assertIn("--memory-registry-dir", runner_text)
        self.assertIn("--engram-pool", runner_text)
        self.assertIn('"$SIM_QWEN3_GUEST_ENGRAM_POOL"', runner_text)
        self.assertIn("memory_decision_reuse=1", runner_text)
        self.assertIn('exec "$SIM_CLI_BIN" "${cli_args[@]}"', runner_text)
        self.assertIn("eight_node_w5_inference_cluster_summary", runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"', runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"', generic_text)
        self.assertIn("source \"$CONFIG_PATH\"", config_runner_text)
        self.assertIn("--steps N", config_runner_text)
        self.assertIn("--validate-only", config_runner_text)
        self.assertIn("STEPS_OVERRIDE", config_runner_text)
        self.assertIn("validate_w5_cluster_config", config_runner_text)
        self.assertIn("W5 cluster config requires SIM_QWEN3_DENSE_WEIGHTS_PATH", config_runner_text)
        self.assertIn("W5 cluster config weights path is missing", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_OBJECT_STORE requires SIM_W5_MEMORY_DECISION_STORE", config_runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM", config_runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_POOL", config_runner_text)
        self.assertIn("fixed RUN_ID is disabled", config_runner_text)
        self.assertIn("SIM_W5_ALLOW_FIXED_RUN_ID", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_OBSERVATION_STORE", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_REUSE_RUN_ID", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_REUSE_OUT_DIR", config_runner_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_OBJECT_STORE", config_runner_text)
        self.assertIn("unset SIM_W5_MEMORY_REUSE_RUN_ID", config_runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"', config_runner_text)
        self.assertIn("explicit obmm cluster runtime bootstrap", legacy_runner_text)
        self.assertIn("SIM_W4_DB_LAZY_REMOTE_ACTIVATION=0", legacy_runner_text)
        self.assertIn("idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE", legacy_runner_text)
        self.assertIn("source=runtime_token_input target=uapi_segment", legacy_runner_text)
        self.assertIn("w4_guest_run_summary.py", summary_text)
        self.assertIn("qwen3_w5_memory_shortpath_commit", legacy_runner_text)

    def test_w5_cluster_config_runner_loads_env_file_without_dynamic_shell_prefix(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "RUN_ID=test-run",
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                        "SIM_W5_MEMORY_SHORTPATH_EXECUTE=0",
                        "SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP=1",
                        "SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP=1",
                        "SIM_W5_MEMORY_OBSERVATION_STORE=/tmp/w5-memory-store.json",
                        "SIM_W5_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                        "SIM_W5_MEMORY_DECISION_OBJECT_STORE=/tmp/w5-object-store.json",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", "--steps", "3", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertEqual(
            result.stdout.strip().splitlines(),
            [
                "RUN_ID=test-run",
                "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                "SIM_QWEN3_GUEST_ENGRAM=0",
                "SIM_QWEN3_GUEST_ENGRAM_POOL=",
                "SIM_QWEN3_GUEST_DECODE_STEPS=3",
                "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                "SIM_W5_MEMORY_SHORTPATH_EXECUTE=0",
                "SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP=1",
                "SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP=1",
                "SIM_W5_MEMORY_OBSERVATION_STORE=/tmp/w5-memory-store.json",
                "SIM_W5_MEMORY_REUSE_RUN_ID=",
                "SIM_W5_MEMORY_REUSE_OUT_DIR=",
                "SIM_W5_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                "SIM_W5_MEMORY_DECISION_OBJECT_STORE=/tmp/w5-object-store.json",
                "SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=",
            ],
        )

    def test_w5_cluster_config_runner_prints_effective_engram_default(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5-engram.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3-14b",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode", result.stdout)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM=1", result.stdout)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_POOL=obmm", result.stdout)

    def test_w5_cluster_config_runner_resolves_latest_memory_reuse_artifacts(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            old_run = "2026-05-26_01-00-00_w5_qwen3_14b_engram_decode_111"
            new_run = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            failed_run = "2026-05-26_03-00-00_w5_qwen3_14b_engram_decode_333"
            other_profile_run = "2026-05-26_04-00-00_w5_qwen3_0_6b_engram_decode_444"
            for index, run_id in enumerate([old_run, new_run, failed_run, other_profile_run], start=1):
                decision_store = out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json"
                object_store = out_dir / f"w5_object_service_store.{run_id}.json"
                decision_store.write_text("{}", encoding="utf-8")
                object_store.write_text("{}", encoding="utf-8")
                summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
                if run_id != failed_run:
                    summary.write_text(
                        "\n".join(
                            [
                                "summary: decode_steps_expected=16 decode_steps_observed=16 passed_nodes=8/8",
                                f"memory_boundary_observation: observation_id=boundary-observation/{run_id}/step0/node1 status=ok",
                            ]
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                mtime = 1700000000 + index
                os.utime(decision_store, (mtime, mtime))
                os.utime(object_store, (mtime, mtime))
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3-14b",
                        "SIM_W5_MEMORY_REUSE_RUN_ID=latest",
                        f"SIM_W5_MEMORY_REUSE_OUT_DIR={out_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(f"SIM_W5_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{new_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_MEMORY_DECISION_OBJECT_STORE={out_dir}/w5_object_service_store.{new_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={new_run}", result.stdout)

    def test_w5_cluster_config_runner_resolves_named_memory_reuse_run(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt").write_text(
                "\n".join(
                    [
                        "summary: decode_steps_expected=16 decode_steps_observed=16 passed_nodes=8/8",
                        f"memory_boundary_observation: observation_id=boundary-observation/{run_id}/step0/node1 status=ok",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_MEMORY_REUSE_RUN_ID={run_id}",
                        f"SIM_W5_MEMORY_REUSE_OUT_DIR={out_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("config validation passed", result.stderr)

    def test_w5_cluster_config_runner_rejects_named_reuse_run_without_completed_summary(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_MEMORY_REUSE_RUN_ID={run_id}",
                        f"SIM_W5_MEMORY_REUSE_OUT_DIR={out_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("reuse summary is missing completion evidence", result.stderr)

    def test_w5_cluster_config_runner_rejects_ambiguous_memory_reuse_config(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3-14b",
                        "SIM_W5_MEMORY_REUSE_RUN_ID=latest",
                        "SIM_W5_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("SIM_W5_MEMORY_REUSE_RUN_ID cannot be combined", result.stderr)

    def test_w5_cluster_config_runner_rejects_fixed_run_id_for_real_runs(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "RUN_ID=test-run",
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("fixed RUN_ID is disabled", result.stderr)

    def test_w5_cluster_config_runner_validate_only_accepts_complete_memory_reuse_config(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                        "SIM_W5_MEMORY_DECISION_OBJECT_STORE=/tmp/w5-object-store.json",
                        "SIM_W5_MEMORY_SHORTPATH_DECISION_IDS=decision-a,decision-b",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", "--steps", "3", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("config validation passed", result.stderr)

    def test_w5_cluster_config_runner_validate_only_rejects_missing_weights_path(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("W5 cluster config requires SIM_QWEN3_DENSE_WEIGHTS_PATH", result.stderr)

    def test_w5_cluster_config_runner_validate_only_rejects_decision_store_without_selector(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("SIM_W5_MEMORY_DECISION_STORE requires a boundary observation/decision selector", result.stderr)

    def test_w5_cluster_config_runner_validate_only_rejects_invalid_steps_override(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", "--steps", "0", str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--steps must be a positive integer: 0", result.stderr)


if __name__ == "__main__":
    unittest.main()
