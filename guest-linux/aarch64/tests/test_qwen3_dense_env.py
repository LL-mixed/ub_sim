#!/usr/bin/env python3
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class Qwen3DenseEnvTest(unittest.TestCase):
    def write_qwen3_14b_stub_weights(self, model_dir):
        model_dir.mkdir()
        (model_dir / "config.json").write_text(
            json.dumps(
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
            ),
            encoding="utf-8",
        )
        (model_dir / "tokenizer.json").write_text("{}", encoding="utf-8")
        (model_dir / "model.safetensors.index.json").write_text("{}", encoding="utf-8")

    def write_w5_reuse_summary(self, out_dir, run_id, steps=2, missing_boundary=None):
        records = steps * 7
        lines = [
            f"summary: decode_steps_expected={steps} decode_steps_observed={steps} passed_nodes=8/8",
            (
                "memory_boundary_observation_summary: "
                f"records={records} steps={steps}/{steps} "
                "nodes=node1,node2,node3,node4,node5,node6,node7 "
                "targets=node2,node3,node4,node5,node6,node7,node8 "
                "source=w5_guest_range_exit hidden_backend=obmm_shmem"
            ),
        ]
        for step in range(steps):
            for node in range(1, 8):
                if missing_boundary == (step, node):
                    continue
                lines.append(
                    "memory_boundary_observation: "
                    f"phase=range_exit observation_id=boundary-observation/{run_id}/step{step}/node{node} "
                    f"step={step} node=node{node} target=node{node + 1} status=ok"
                )
        lines.append(
            "memory_service_summary: "
            f"service=lingqu_memory_service records={steps * 7} steps={steps}/{steps} "
            "stages=qwen3_w5_memory_boundary_decision:"
            f"{steps * 7},qwen3_w5_memory_terminal_logits_execute:{steps * 7} "
            "shortpath_ids=shortpath_stream support_ids=shortpath_stream "
            "actions=jump-to-terminal artifact_kinds=logits lookup_hits="
            f"{steps * 7}"
        )
        for step in range(steps):
            lines.append(
                "memory_service_step: "
                f"step={step} boundary_records=7 nodes=node1,node2,node3,node4,node5,node6,node7 "
                "shortpath_ids=shortpath_stream support_ids=shortpath_stream "
                "actions=jump-to-terminal lookup_hits=7"
            )
        (out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt").write_text(
            "\n".join(lines) + "\n",
            encoding="utf-8",
        )

    def write_w5_boundary_only_summary(self, out_dir, run_id, steps=2):
        records = steps * 7
        lines = [
            f"summary: decode_steps_expected={steps} decode_steps_observed={steps} passed_nodes=8/8",
            (
                "memory_boundary_observation_summary: "
                f"records={records} steps={steps}/{steps} "
                "nodes=node1,node2,node3,node4,node5,node6,node7 "
                "targets=node2,node3,node4,node5,node6,node7,node8 "
                "source=w5_guest_range_exit hidden_backend=obmm_shmem"
            ),
        ]
        for step in range(steps):
            for node in range(1, 8):
                lines.append(
                    "memory_boundary_observation: "
                    f"phase=range_exit observation_id=boundary-observation/{run_id}/step{step}/node{node} "
                    f"step={step} node=node{node} target=node{node + 1} status=ok"
                )
        (out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt").write_text(
            "\n".join(lines) + "\n",
            encoding="utf-8",
        )

    def write_w5_shortpath_only_summary(self, out_dir, run_id, steps=2):
        lines = [
            f"summary: decode_steps_expected={steps} decode_steps_observed={steps} passed_nodes=8/8",
            (
                "memory_service_summary: service=lingqu_memory_service "
                f"records={steps} steps={steps}/{steps} actions=jump-to-terminal "
                f"lookup_hits={steps}"
            ),
            (
                "memory_boundary_observations_recorded: "
                "records=0 status=skipped reason=shortpath_no_range_exit"
            ),
        ]
        (out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt").write_text(
            "\n".join(lines) + "\n",
            encoding="utf-8",
        )

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
        qwen3_0_6b_wrapper = script_dir / "run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh"

        self.assertTrue(wrapper.exists())
        self.assertTrue(wrapper.stat().st_mode & 0o111)
        self.assertTrue(qwen3_0_6b_wrapper.exists())
        self.assertTrue(qwen3_0_6b_wrapper.stat().st_mode & 0o111)

        text = wrapper.read_text(encoding="utf-8")
        qwen3_0_6b_text = qwen3_0_6b_wrapper.read_text(encoding="utf-8")
        self.assertIn("SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}", text)
        self.assertIn("SIM_QWEN3_GUEST_DECODE_STEPS:-2", text)
        self.assertIn("SIM_QWEN3_DENSE_WEIGHTS_PATH:-", text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"', text)
        self.assertIn("SIM_QWEN3_0_6B_WEIGHTS_PATH:-/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B", qwen3_0_6b_text)
        self.assertIn("SIM_QWEN3_GUEST_DECODE_STEPS:-2", qwen3_0_6b_text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"', qwen3_0_6b_text)

    def test_eight_node_runner_passes_decode_round_barrier_timeout(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        runner = script_dir / "run_llm_infer_eight_node_guest.sh"
        launcher = script_dir / "launch_ub_eight_node_headless.sh"

        runner_text = runner.read_text(encoding="utf-8")
        launcher_text = launcher.read_text(encoding="utf-8")

        self.assertIn("SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}", runner_text)
        self.assertIn("SIM_UAPI_W5_PROFILE", runner_text)
        self.assertIn("w5_profile_default_w4_backend", runner_text)
        self.assertIn("validate_w5_profile_runtime", runner_text)
        self.assertIn("SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS", runner_text)
        self.assertIn("APP_WAIT_SECS * 1000", runner_text)
        self.assertIn("SIM_QWEN3_RUNTIME_RANGE_WAIT_MS", runner_text)
        self.assertIn(
            "APP_WAIT_SECS * ${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-$SIM_QWEN3_GUEST_DECODE_STEPS} * 1000",
            runner_text,
        )
        self.assertIn("SIM_UAPI_W5_PROFILE", launcher_text)
        self.assertIn("SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS", launcher_text)
        self.assertIn("SIM_QWEN3_RUNTIME_RANGE_WAIT_MS", launcher_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_STATE_REF", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST", launcher_text)
        self.assertIn(
            'SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST"',
            runner_text,
        )
        self.assertIn(
            'SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT" \\',
            runner_text,
        )
        self.assertIn(
            'SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST" \\',
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
        self.assertIn("guest_worker_shortpath_summary: action=jump-to-terminal", runner_text)
        self.assertIn("boundary_hits=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn("terminal_selects=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn("expected_hits=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn("actual_runtime_inputs=$((SIM_QWEN3_GUEST_DECODE_STEPS - 1))", runner_text)
        self.assertIn("actual_runtime_outputs=0", runner_text)
        self.assertIn("shortpath_no_dispatch=${idle_expected}", runner_text)
        self.assertIn("shortpath_terminal_commits=${idle_expected}", runner_text)
        self.assertIn("shortpath_publish_hidden_zero=${SIM_QWEN3_GUEST_DECODE_STEPS}", runner_text)
        self.assertIn("full_pipeline_range_forwards=$((SIM_QWEN3_GUEST_DECODE_STEPS * ${#NODE_IDS[@]}))", runner_text)
        self.assertIn("full_pipeline_runtime_inputs=$((SIM_QWEN3_GUEST_DECODE_STEPS * ${#NODE_IDS[@]} - 1))", runner_text)
        self.assertIn("full_pipeline_runtime_outputs=$((SIM_QWEN3_GUEST_DECODE_STEPS * ${#NODE_IDS[@]}))", runner_text)
        self.assertIn("W5 shortpath worker summary does not prove reduced range pipeline", runner_text)
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
        self.assertIn("emit_w5_inference_run_report", runner_text)
        self.assertIn("w5_inference_run_report.py", runner_text)
        self.assertIn("W5 inference run report validation failed", runner_text)
        self.assertIn("--validate-w5-artifact-sizes-only", runner_text)
        self.assertIn('TEE_BIN="${TEE_BIN:-/usr/bin/tee}"', runner_text)
        self.assertIn("zstat -H file_stat +size", runner_text)
        self.assertIn('$shortpath_kv_stream" == /tmp/*', runner_text)
        self.assertIn("SIM_W5_TEST_MAX_MEMORY_STORE_JSON_BYTES:-16777216", runner_text)
        self.assertIn("SIM_W5_TEST_MAX_OBJECT_STORE_JSON_BYTES:-8388608", runner_text)
        self.assertIn("SIM_W5_TEST_MAX_OBJECT_STORE_BIN_BYTES:-268435456", runner_text)
        self.assertIn("compute_w5_object_store_bin_max_bytes", runner_text)
        self.assertIn("per_step_bytes=$((24 * 1024 * 1024))", runner_text)
        self.assertIn("SIM_W5_TEST_MAX_SHORTPATH_STREAM_BYTES:-1048576", runner_text)
        self.assertIn("SIM_W5_TEST_MAX_SHORTPATH_KV_STREAM_BYTES:-1048576", runner_text)
        self.assertIn("SIM_W5_TEST_MAX_PREFIX_CACHE_KV_STREAM_BYTES:-1048576", runner_text)
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
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_STORE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_STORE", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT", launcher_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH", launcher_text)

    def test_w5_artifact_size_validation_cli_uses_host_registry_for_guest_tmp_streams(self):
        runner = Path(__file__).resolve().parents[1] / "scripts" / "run_llm_infer_eight_node_guest.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            registry_dir = tmp_path / "registry"
            registry_dir.mkdir()
            memory_store = tmp_path / "memory_store.json"
            object_store = tmp_path / "object_store.json"
            object_bin = tmp_path / "object_store.bin"
            shortpath_stream = registry_dir / "w5_memory_shortpath_stream.txt"
            shortpath_kv_stream = registry_dir / "w5_memory_shortpath_kv_stream.txt"
            prefix_cache_kv_stream = registry_dir / "w5_memory_prefix_cache_kv_stream.txt"

            for path in (
                memory_store,
                object_store,
                object_bin,
                shortpath_stream,
                shortpath_kv_stream,
                prefix_cache_kv_stream,
            ):
                path.write_bytes(b"ok")

            env = os.environ.copy()
            env.update(
                {
                    "SIM_UAPI_W5_PROFILE": "qwen3_14b_engram_decode",
                    "SIM_W5_MEMORY_STORE": str(memory_store),
                    "SIM_W5_MEMORY_OBJECT_STORE": str(object_store),
                    "SIM_W5_MEMORY_REGISTRY_DIR": str(registry_dir),
                    "SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH": "/tmp/w5_memory_shortpath_stream.txt",
                    "SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH": "/tmp/w5_memory_shortpath_kv_stream.txt",
                    "SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH": "/tmp/w5_memory_prefix_cache_kv_stream.txt",
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
            self.assertIn(f"label=prefix_cache_kv_stream bytes=2", result.stderr)
            self.assertIn(str(prefix_cache_kv_stream), result.stderr)
            self.assertNotIn("/tmp/w5_memory_shortpath_kv_stream.txt", result.stderr)
            self.assertNotIn("/tmp/w5_memory_prefix_cache_kv_stream.txt", result.stderr)

    def test_w5_artifact_size_validation_cli_allows_prefix_cache_without_shortpath_streams(self):
        runner = Path(__file__).resolve().parents[1] / "scripts" / "run_llm_infer_eight_node_guest.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            registry_dir = tmp_path / "registry"
            registry_dir.mkdir()
            memory_store = tmp_path / "memory_store.json"
            object_store = tmp_path / "object_store.json"
            object_bin = tmp_path / "object_store.bin"
            prefix_cache_kv_stream = registry_dir / "w5_memory_prefix_cache_kv_stream.txt"

            for path in (memory_store, object_store, object_bin, prefix_cache_kv_stream):
                path.write_bytes(b"ok")

            env = os.environ.copy()
            env.update(
                {
                    "SIM_UAPI_W5_PROFILE": "qwen3_14b_engram_decode",
                    "SIM_W5_MEMORY_STORE": str(memory_store),
                    "SIM_W5_MEMORY_OBJECT_STORE": str(object_store),
                    "SIM_W5_MEMORY_REGISTRY_DIR": str(registry_dir),
                    "SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH": "/tmp/w5_memory_prefix_cache_kv_stream.txt",
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

            self.assertIn(f"label=prefix_cache_kv_stream bytes=2", result.stderr)
            self.assertIn(str(prefix_cache_kv_stream), result.stderr)
            self.assertNotIn("FAIL: W5 artifact size check missing label=shortpath_stream", result.stderr)
            self.assertNotIn("FAIL: W5 artifact size check missing label=shortpath_kv_stream", result.stderr)

    def test_w5_artifact_size_validation_cli_fails_on_oversized_artifact(self):
        runner = Path(__file__).resolve().parents[1] / "scripts" / "run_llm_infer_eight_node_guest.sh"

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
                    "SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH": "/tmp/w5_memory_shortpath_kv_stream.txt",
                    "SIM_W5_TEST_MAX_SHORTPATH_KV_STREAM_BYTES": "1",
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

    def test_w5_artifact_size_validation_cli_scales_object_bin_limit_by_decode_steps(self):
        runner = Path(__file__).resolve().parents[1] / "scripts" / "run_llm_infer_eight_node_guest.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            registry_dir = tmp_path / "registry"
            registry_dir.mkdir()
            memory_store = tmp_path / "memory_store.json"
            memory_bin = tmp_path / "memory_store.bin"
            object_store = tmp_path / "object_store.json"
            object_bin = tmp_path / "object_store.bin"
            shortpath_stream = registry_dir / "w5_memory_shortpath_stream.txt"
            shortpath_kv_stream = registry_dir / "w5_memory_shortpath_kv_stream.txt"

            for path in (memory_store, object_store, object_bin, shortpath_stream, shortpath_kv_stream):
                path.write_bytes(b"ok")
            with memory_bin.open("wb") as f:
                f.truncate(300 * 1024 * 1024)

            env = os.environ.copy()
            env.update(
                {
                    "SIM_UAPI_W5_PROFILE": "qwen3_14b_engram_decode",
                    "SIM_QWEN3_GUEST_DECODE_STEPS": "16",
                    "SIM_W5_MEMORY_STORE": str(memory_store),
                    "SIM_W5_MEMORY_OBJECT_STORE": str(object_store),
                    "SIM_W5_MEMORY_REGISTRY_DIR": str(registry_dir),
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

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("label=memory_store_bin", result.stderr)
            self.assertIn("max_bytes=402653184", result.stderr)

    def test_guest_consumes_w5_prefix_cache_reuse_as_kv_object_ref(self):
        guest_source = (
            Path(__file__).resolve().parents[1]
            / "apps"
            / "llm_infer"
            / "llm_infer.c"
        ).read_text(encoding="utf-8")
        mem_service_root = Path(
            os.environ.get(
                "MEM_SERVICE_ROOT", Path(__file__).resolve().parents[3] / "mem_service"
            )
        )
        db_service_dir = mem_service_root / "components" / "mem_service"
        db_service_source = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(
                {
                    db_service_dir / "mem_service_module.c",
                    *db_service_dir.glob("mem_service_*.c"),
                    *db_service_dir.glob("mem_service_*.inc"),
                }
            )
        )
        db_service_header = (
            mem_service_root / "components" / "mem_service" / "mem_service.h"
        ).read_text(encoding="utf-8")
        cli_source = (
            Path(__file__).resolve().parents[3] / "crates" / "sim-cli" / "src" / "main.rs"
        ).read_text(encoding="utf-8")
        eight_node_runner = (
            Path(__file__).resolve().parents[1]
            / "scripts"
            / "run_llm_infer_eight_node_guest.sh"
        ).read_text(encoding="utf-8")

        self.assertIn("qwen3_memory_prefix_cache_kv_ref", guest_source)
        self.assertIn("qwen3_memory_prefix_cache_partial_prefill_active", guest_source)
        self.assertIn("SIM_W5_SERVING_REQUEST_ID", guest_source)
        self.assertIn("qwen3_memory_serving_request_id", guest_source)
        self.assertIn("request_id=%s", guest_source)
        self.assertIn('SIM_W5_SERVING_REQUEST_ID="${SIM_W5_SERVING_REQUEST_ID:-}"', eight_node_runner)
        self.assertIn('export SIM_W5_SERVING_REQUEST_ID="$SIM_W5_SERVING_REQUEST_ID"', eight_node_runner)
        self.assertIn('SIM_W5_SERVING_REQUEST_ID="$SIM_W5_SERVING_REQUEST_ID"', eight_node_runner)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF", guest_source)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS", guest_source)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS", guest_source)
        self.assertIn("qwen3_w5_memory_prefix_cache_suffix_replay_token", guest_source)
        self.assertNotIn("partial prefix-cache suffix unsupported", guest_source)
        self.assertIn("W4_QWEN3_OBMM_KIND_QWEN3_KV_STATE", guest_source)
        self.assertIn("qwen3_w5_memory_prefix_cache_kv_loaded", guest_source)
        self.assertIn("source=lingqu_memory_service target=uapi_object_ref", guest_source)
        self.assertIn("jump-to-terminal", guest_source)
        self.assertIn("artifact_kind=%s", guest_source)
        self.assertIn("jump-to-terminal contract invalid", guest_source)
        self.assertIn("qwen3_memory_shortpath_terminal_logits_record", guest_source)
        self.assertIn("qwen3_read_object_service_payload", guest_source)
        self.assertIn("W4_QWEN3_OBJECT_SERVICE_PAYLOAD_INDEX_MAGIC", guest_source)
        self.assertIn("qwen3_find_logits_table_by_scan_for_step", guest_source)
        self.assertIn("qwen3_logits_table_candidate_matches_step", guest_source)
        self.assertIn("base + i * entry_bytes + 72ULL", guest_source)
        self.assertIn("qwen3_serving_effective_decode_step", guest_source)
        self.assertIn("uint64_t logits_expected_decode_step =", guest_source)
        self.assertIn("qwen3_serving_effective_decode_step(decode_step);", guest_source)
        self.assertIn(
            "qwen3_find_logits_table_by_scan_for_step(ep_mmio,\n"
            "                                                          true,\n"
            "                                                          logits_expected_decode_step,",
            guest_source,
        )
        self.assertIn(
            "range_only_flow ? logits_expected_decode_step : entry",
            guest_source,
        )
        self.assertIn("mem_service_obmm_service_v0_ensure_cluster_runtime", guest_source)
        self.assertIn("obmm_cluster_runtime_bootstrap", db_service_source)
        self.assertIn("mem_service_cluster_runtime_require", db_service_source)
        self.assertIn("lazy_activation_forbidden", db_service_source)
        self.assertIn("peer_not_bootstrapped", db_service_source)
        self.assertIn("after=obmm_cluster_runtime_bootstrap", guest_source)
        self.assertIn(
            "needs_engram_history =\n                    local_decode_node == qwen3_engram_config.owner_node",
            guest_source,
        )
        self.assertNotIn("local_decode_node == 0U ||", guest_source)
        self.assertNotIn("local_decode_node + 1U == cluster_node_count ||", guest_source)
        self.assertIn("mem_service_take_pending_object_desc", db_service_source)
        self.assertIn("mem_service_take_pending_object_kind_len_desc", db_service_source)
        self.assertIn("qwen3_w5_memory_terminal_logits_loaded", guest_source)
        self.assertIn(
            "mem_service_obmm_service_v0_publish_shortpath_terminal_token_result",
            guest_source,
        )
        self.assertIn("uint32_t creator_node;", guest_source)
        self.assertIn("shortpath_kv_stream_creator_node", guest_source)
        self.assertIn("prefix_cache_kv_stream_creator_node", guest_source)
        self.assertNotIn("creator_node == local_node + 1U", guest_source)
        self.assertIn("entry->target_layer_start == layer_start", guest_source)
        self.assertIn("candidate->target_layer_start == layer_start", guest_source)
        self.assertIn("runtime_kv_checksum = w4_qwen3_hidden_payload_checksum", guest_source)
        self.assertIn("runtime_checksum=0x%016", guest_source)
        self.assertIn("object_checksum =\n            qwen3_lingqu_object_payload_checksum", guest_source)
        self.assertIn("runtime_checksum =\n            w4_qwen3_hidden_payload_checksum", guest_source)
        self.assertIn("object_checksum != ref->payload_checksum &&", guest_source)
        self.assertIn(
            "expected_entry_step =\n                    range_only_flow ? "
            "logits_expected_decode_step : entry",
            guest_source,
        )
        self.assertIn(
            "expected_step =\n                    range_only_flow ? "
            "logits_expected_decode_step : entry",
            guest_source,
        )
        self.assertIn(
            "expected_boundary_first =\n                range_only_flow && "
            "logits_expected_decode_step != 0 ? 0ULL : 1ULL",
            guest_source,
        )
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
        self.assertNotIn("qwen3_memory_shortpath_downstream_kv_support_complete", guest_source)
        self.assertNotIn("skipped_downstream_kv_state_unavailable", guest_source)
        self.assertNotIn("shortpath_execution_guard", guest_source)
        self.assertIn("qwen3_memory_shortpath_validate_live_boundary_match", guest_source)
        self.assertIn("qwen3_w5_memory_shortpath_approximate_match", guest_source)
        self.assertIn("qwen3_round_decode_position", guest_source)
        self.assertIn(
            "qwen3_prompt_base_token_count + guest_decode_step",
            guest_source,
        )
        self.assertIn("qwen3_decode_position_resolved", guest_source)
        self.assertIn(
            "qwen3_memory_service_lookup_boundary(\n"
            "        memory_config,\n"
            "        dispatch_node,\n"
            "        cluster_node_count,\n"
            "        layer_start,\n"
            "        layer_end,\n"
            "        decode_step,\n"
            "        position,",
            guest_source,
        )
        self.assertIn("mem_service_obmm_service_v0_publish_runtime_range_kv_state", guest_source)
        self.assertIn("qwen3_decode_round_scheduler_no_dispatch", guest_source)
        self.assertIn("work_item=none", guest_source)
        self.assertIn("dispatch=skipped status=no_dispatch", guest_source)
        self.assertIn("qwen3_work_item_scheduler_wait", guest_source)
        self.assertIn("qwen3_work_item_scheduler_dispatch", guest_source)
        self.assertIn("mem_service_range_flow_wait_scheduler_work_item", guest_source)
        self.assertIn("w4_runtime_init_obmm_range_flow_request", guest_source)
        self.assertIn("struct mem_service_scheduler_work_item", db_service_header)
        self.assertIn("MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD", db_service_header)
        self.assertIn("MEM_SERVICE_SCHEDULER_WORK_ITEM_NO_DISPATCH", db_service_header)
        self.assertIn("qwen3_memory_service_boundary_lookup_request", guest_source)
        self.assertIn("qwen3_memory_service_boundary_lookup_response", guest_source)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF", guest_source)
        self.assertIn("qwen3_read_memory_boundary_registry_object", guest_source)
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
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE", guest_source)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND", guest_source)
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
        self.assertIn("mem_service_take_pending_token_result_desc", db_service_source)
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
        self.assertIn("mem_service_obmm_service_v0_try_resolve_range_kv_state_view", guest_source)
        self.assertIn("mem_service_qwen3_format_kv_state_key", db_service_source)
        self.assertIn("kvcache/%s/scope/%016", db_service_source)
        self.assertIn("mem_service_qwen3_format_token_result_key", db_service_source)
        self.assertIn("tokens/%s/scope/%016", db_service_source)
        self.assertIn("mem_service_qwen3_format_runtime_range_key", db_service_source)
        self.assertIn("hidden/%s/scope/%016", db_service_source)
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
        runner = script_dir / "run_w5_inference_cluster_runtime.sh"
        config_runner = script_dir / "run_w5_cluster_config.sh"
        realistic_matrix_runner = script_dir / "run_w5_prefix_cache_realistic_matrix.sh"
        serving_matrix_runner = script_dir / "run_w5_prefix_cache_serving_matrix.sh"
        stable_w5_runner = script_dir / "run_w5_cluster_qwen3_0_6b_2step.sh"
        summary = script_dir / "w5_inference_cluster_summary.py"
        launcher = script_dir / "launch_ub_eight_node_headless.sh"
        build_initramfs = script_dir / "build_initramfs.sh"

        self.assertTrue(runner.exists())
        self.assertTrue(runner.stat().st_mode & 0o111)
        self.assertFalse((script_dir / "run_ub_w5_inference_cluster.sh").exists())
        self.assertTrue(config_runner.exists())
        self.assertTrue(config_runner.stat().st_mode & 0o111)
        self.assertTrue(realistic_matrix_runner.exists())
        self.assertTrue(realistic_matrix_runner.stat().st_mode & 0o111)
        self.assertTrue(serving_matrix_runner.exists())
        self.assertTrue(serving_matrix_runner.stat().st_mode & 0o111)
        self.assertTrue(summary.exists())
        self.assertTrue(summary.stat().st_mode & 0o111)

        runner_text = runner.read_text(encoding="utf-8")
        config_runner_text = config_runner.read_text(encoding="utf-8")
        realistic_matrix_runner_text = realistic_matrix_runner.read_text(encoding="utf-8")
        serving_matrix_runner_text = serving_matrix_runner.read_text(encoding="utf-8")
        stable_w5_runner_text = stable_w5_runner.read_text(encoding="utf-8")
        legacy_runner_text = (script_dir / "run_llm_infer_eight_node_guest.sh").read_text(encoding="utf-8")
        w4_compat_runner_text = (script_dir / "run_ub_eight_node_w4_guest.sh").read_text(encoding="utf-8")
        launcher_text = launcher.read_text(encoding="utf-8")
        summary_text = summary.read_text(encoding="utf-8")
        build_initramfs_text = build_initramfs.read_text(encoding="utf-8")

        self.assertIn("SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-1", runner_text)
        self.assertNotIn("DEMO_WAIT_SECS", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP:-1", runner_text)
        self.assertIn("SIM_W5_TEST_REQUIRE_PREFIX_CACHE", runner_text)
        self.assertIn("SIM_W5_TEST_REQUIRE_PREFIX_CACHE:-0", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_OBSERVATION_STORE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_OUT_DIR", runner_text)
        self.assertIn("SIM_W5_TEST_VALIDATE_ONLY", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_STORE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF", legacy_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT", legacy_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID", runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_IDS", runner_text)
        self.assertIn("SIM_W5_MEMORY_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_OBJECT_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_ENGRAM_STATE", runner_text)
        self.assertIn("SIM_W5_MEMORY_STORE", legacy_runner_text)
        self.assertIn("SIM_W5_MEMORY_OBJECT_STORE", legacy_runner_text)
        self.assertIn("SIM_W5_MEMORY_STORE", launcher_text)
        self.assertIn("SIM_W5_MEMORY_OBJECT_STORE", launcher_text)
        self.assertIn("nohup env", launcher_text)
        self.assertIn('disown "$qemu_pid"', launcher_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_POOL", runner_text)
        self.assertIn("SIM_W5_MEMORY_REGISTRY_DIR", runner_text)
        self.assertIn("target/debug/sim-cli", runner_text)
        self.assertIn("cargo build -p sim-cli", runner_text)
        self.assertIn("unset SIM_CLI_BIN so the runner builds", runner_text)
        self.assertIn("--memory-runtime-boundary-lookup", runner_text)
        self.assertIn("--memory-post-run-promote", runner_text)
        self.assertIn("--memory-online-boundary-lookup", runner_text)
        self.assertIn("--memory-prefix-cache-lookup=true", runner_text)
        self.assertIn("--memory-prefix-cache-lookup=false", runner_text)
        self.assertIn('lingqu-memory prefix-cache-service', runner_text)
        self.assertIn("--memory-observation-store", runner_text)
        self.assertIn("--memory-store", runner_text)
        self.assertIn("--memory-object-store", runner_text)
        self.assertIn("--memory-engram-state", runner_text)
        self.assertIn("--memory-registry-dir", runner_text)
        self.assertIn("--memory-decision-store", runner_text)
        self.assertIn("--validate-only", runner_text)
        self.assertIn("--memory-decision-object-store", runner_text)
        self.assertIn("--memory-boundary-observation-run-id", runner_text)
        self.assertIn("--memory-shortpath-decision-ids", runner_text)
        self.assertIn("serving_queue=1 launch_mode=ready_only", runner_text)
        self.assertIn("--memory-store", runner_text)
        self.assertIn("--memory-object-store", runner_text)
        self.assertIn("--memory-engram-state", runner_text)
        self.assertIn("--memory-registry-dir", runner_text)
        self.assertIn("--engram-pool", runner_text)
        self.assertIn('"$SIM_QWEN3_GUEST_ENGRAM_POOL"', runner_text)
        self.assertIn("memory_decision_reuse=1", runner_text)
        self.assertIn('"${SIM_CLI_BIN}" "${cli_args[@]}"', runner_text)
        self.assertIn("stop_prefix_cache_service", runner_text)
        self.assertIn("eight_node_w5_inference_cluster_summary", runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_llm_infer_eight_node_guest.sh"', runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_llm_infer_eight_node_guest.sh" "$@"', w4_compat_runner_text)
        self.assertIn("source \"$CONFIG_PATH\"", config_runner_text)
        self.assertIn("--readiness-only", config_runner_text)
        self.assertIn("deepseek_v4_flash_decode", config_runner_text)
        self.assertIn("is_deepseek_v4_flash_w5_profile", config_runner_text)
        self.assertIn("--profile=deepseek-v4-flash", config_runner_text)
        self.assertIn("deepseek-v4-flash-moe-report", config_runner_text)
        self.assertIn("--steps N", config_runner_text)
        self.assertIn("--requests FILE", config_runner_text)
        self.assertIn("--serve-queue", config_runner_text)
        self.assertIn("--serve-requests FILE", config_runner_text)
        self.assertIn("--validate-only", config_runner_text)
        self.assertIn("W5 cluster config file is required", config_runner_text)
        self.assertIn("run_w5_cluster_qwen3_0_6b_2step.sh", config_runner_text)
        self.assertNotIn("DEFAULT_CONFIG=", config_runner_text)
        self.assertIn("deepseek_v4_flash_decode)", runner_text)
        self.assertIn("deepseek-v4-flash", runner_text)
        self.assertIn("deepseek_v4_flash_decode)", legacy_runner_text)
        self.assertIn("echo deepseek-v4-flash", legacy_runner_text)
        self.assertIn("deepseek-v4-flash-simpler", legacy_runner_text)
        self.assertIn(
            'is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"',
            legacy_runner_text,
        )
        self.assertIn(
            "deepseek-v4-flash-simpler|deepseek_v4_flash_simpler|deepseek-v4-flash-official",
            runner_text,
        )
        self.assertIn('--model "$SIM_DEEPSEEK_V4_FLASH"', runner_text)
        self.assertNotIn('--model "$deepseek_runtime_dir/ds4flash.gguf"', runner_text)
        self.assertIn("--gsva-kv", config_runner_text)
        self.assertIn("--require-prefix-cache", config_runner_text)
        self.assertIn("--no-memory-reuse", config_runner_text)
        self.assertIn("STEPS_OVERRIDE", config_runner_text)
        self.assertIn("GSVA_KV_OVERRIDE", config_runner_text)
        self.assertIn("REQUIRE_PREFIX_CACHE_OVERRIDE", config_runner_text)
        self.assertIn("DISABLE_MEMORY_REUSE_OVERRIDE", config_runner_text)
        self.assertIn("validate_w5_cluster_config", config_runner_text)
        self.assertNotIn("DEMO_WAIT_SECS", config_runner_text)
        self.assertIn("W5 cluster config requires SIM_QWEN3_DENSE_WEIGHTS_PATH", config_runner_text)
        self.assertIn("W5 cluster config weights path is missing", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE requires SIM_W5_TEST_MEMORY_DECISION_STORE", config_runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM", config_runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_POOL", config_runner_text)
        self.assertIn("SIM_W5_PROGRESS_INTERVAL_SECS", config_runner_text)
        self.assertIn("fixed RUN_ID is disabled", config_runner_text)
        self.assertIn("SIM_W5_ALLOW_FIXED_RUN_ID", config_runner_text)
        self.assertIn("reject_deprecated_w5_env", config_runner_text)
        self.assertIn("reject_deprecated_w5_env_var SIM_W5_MEMORY_DECISION_STORE SIM_W5_TEST_MEMORY_DECISION_STORE", config_runner_text)
        self.assertIn("reject_deprecated_w5_env_var SIM_W5_MEMORY_GSVA_KV SIM_W5_TEST_MEMORY_GSVA_KV", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-1", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP:-1", config_runner_text)
        self.assertIn("SIM_W5_TEST_REQUIRE_PREFIX_CACHE", config_runner_text)
        self.assertIn("SIM_W5_SERVING_REQUESTS_FILE", config_runner_text)
        self.assertIn("SIM_W5_SERVING_QUEUE", config_runner_text)
        self.assertIn("SIM_W5_SERVING_INGRESS", config_runner_text)
        self.assertIn("SIM_W5_SERVING_SUBMIT_REQUESTS_FILE", config_runner_text)
        self.assertIn("w5_serving_entry.py", config_runner_text)
        self.assertIn("SERVING_CONTROL_APP_SRC", build_initramfs_text)
        self.assertIn("linqu_w5_serving_control", build_initramfs_text)
        self.assertIn("SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE=1", stable_w5_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=1", stable_w5_runner_text)
        self.assertIn("SIM_W5_PROGRESS_INTERVAL_SECS", stable_w5_runner_text)
        self.assertNotIn("printf 'W4_GUEST_PROGRESS_INTERVAL_SECS=", stable_w5_runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_w5_cluster_config.sh" "$CONFIG_PATH"', stable_w5_runner_text)
        self.assertIn("--reuse-runs N", realistic_matrix_runner_text)
        self.assertIn("--no-memory-reuse", realistic_matrix_runner_text)
        self.assertIn("--require-prefix-cache", realistic_matrix_runner_text)
        self.assertIn("--compare-prefix-cache-benefit", realistic_matrix_runner_text)
        self.assertIn("run_w5_cluster_config.sh", realistic_matrix_runner_text)
        self.assertIn("w5_inference_run_report.py", realistic_matrix_runner_text)
        self.assertIn("--same-prefix-runs N", serving_matrix_runner_text)
        self.assertIn("--shared-prefix-token-ids CSV", serving_matrix_runner_text)
        self.assertIn("--suffix-b-token-ids CSV", serving_matrix_runner_text)
        self.assertIn("--expect-prefix-cache-matched-tokens", serving_matrix_runner_text)
        self.assertIn("--expect-prefix-cache-suffix-replay-tokens", serving_matrix_runner_text)
        self.assertIn("multi-token suffixes", serving_matrix_runner_text)
        self.assertIn("expect_fail_closed=true", serving_matrix_runner_text)
        self.assertIn("SIM_LLM_INFER_PROMPT_TOKEN_IDS", serving_matrix_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_OUT_DIR", serving_matrix_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE", serving_matrix_runner_text)
        self.assertIn('write_case_config shared-prefix-seed "$SHARED_PREFIX_TOKEN_IDS"', serving_matrix_runner_text)
        self.assertIn('write_case_config request-b "$prompt_b" "$OUT_DIR" "$seed_run_id" 0 0', serving_matrix_runner_text)
        self.assertIn('include_boundary_selector="${6:-1}"', serving_matrix_runner_text)
        self.assertIn("stage_w5_serving_requests_file", legacy_runner_text)
        self.assertIn("run_serving_requests_file", legacy_runner_text)
        self.assertIn("run_serving_stdin_queue", legacy_runner_text)
        self.assertIn("run_serving_nodea_worker_queue", legacy_runner_text)
        self.assertIn("serving_entry ready mode=serial-line", legacy_runner_text)
        self.assertIn("serving_entry ready mode=nodeA-worker", legacy_runner_text)
        self.assertIn("serving_entry request_publish source=nodeA", legacy_runner_text)
        self.assertIn("serving_entry worker_received", legacy_runner_text)
        self.assertIn("/bin/linqu_w5_serving_control publish", legacy_runner_text)
        self.assertIn("/bin/linqu_w5_serving_control wait", legacy_runner_text)
        self.assertIn("--request-index", legacy_runner_text)
        self.assertIn("SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB=4096", legacy_runner_text)
        self.assertIn("while :; do", legacy_runner_text)
        self.assertIn('request_index=\\$((request_index + 1))', legacy_runner_text)
        self.assertIn("done < /dev/ttyAMA0", legacy_runner_text)
        self.assertIn("W5 serving queue ready", legacy_runner_text)
        self.assertIn("run_w5_serving_submit.sh", legacy_runner_text)
        self.assertIn("--fanout nodeA --wait-targets cluster", legacy_runner_text)
        self.assertIn("W5 serving requests completed", legacy_runner_text)
        self.assertIn("w5_serving_object_service_store.${RUN_ID_BASE}.json", legacy_runner_text)
        self.assertIn('-z "$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT"', legacy_runner_text)
        self.assertIn('export SIM_W5_TEST_REQUIRE_PREFIX_CACHE="$SIM_W5_TEST_REQUIRE_PREFIX_CACHE"', legacy_runner_text)
        self.assertIn("serving_entry request_start", legacy_runner_text)
        self.assertIn("SIM_W5_SERVING_DECODE_STEPS_TOTAL", legacy_runner_text)
        self.assertIn("kvcache/qwen3[-.0-9a-z]*(/scope/", legacy_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_OBSERVATION_STORE", config_runner_text)
        self.assertIn("SIM_W5_TEST_VALIDATE_ONLY", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_OUT_DIR", config_runner_text)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE", config_runner_text)
        self.assertIn("--post-run-prune", config_runner_text)
        self.assertIn("--post-run-health", config_runner_text)
        self.assertIn("SIM_W5_TEST_POST_RUN_PRUNE", config_runner_text)
        self.assertIn("SIM_W5_TEST_POST_RUN_HEALTH", config_runner_text)
        self.assertIn("SIM_W5_TEST_ARTIFACT_KEEP_LATEST", config_runner_text)
        self.assertIn("w5_artifact_prune.py", config_runner_text)
        self.assertIn("w5_cluster_health_check.py", config_runner_text)
        self.assertIn("unset SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG", config_runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_w5_inference_cluster_runtime.sh"', config_runner_text)
        self.assertIn("explicit obmm cluster runtime bootstrap", legacy_runner_text)
        self.assertIn('SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION:-0', legacy_runner_text)
        self.assertNotIn('SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION:-1', legacy_runner_text)
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
                        "SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE=0",
                        "SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP=1",
                        "SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE=1",
                        "SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP=1",
                        "SIM_W5_TEST_MEMORY_OBSERVATION_STORE=/tmp/w5-memory-store.json",
                        "SIM_W5_TEST_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                        "SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE=/tmp/w5-object-store.json",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                        "SIM_ENGRAM_SIMT_ARTIFACT_DIR=/tmp/engram-simt",
                        "SIM_ENGRAM_SIMT_SELECTED_SYMBOL=engram_context_dim8_b1",
                        "SIM_ENGRAM_SIMT_SELECTED_CASE=dim8_batch1",
                        "SIM_ENGRAM_SIMT_BINARY_PATH=/tmp/engram-simt/engram-simt",
                        "SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH=/tmp/engram-simt/libkernel.so",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", "--gsva-kv", "--steps", "3", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertEqual(
            result.stdout.strip().splitlines(),
            [
                "# runtime",
                "RUN_ID=test-run",
                "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                "SIM_W5_CLUSTER_NODE_COUNT=8",
                "SIM_QWEN3_GUEST_DECODE_STEPS=3",
                "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                "SIM_QWEN3_GUEST_ENGRAM=0",
                "SIM_QWEN3_GUEST_ENGRAM_POOL=",
                "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                "SIM_W5_PROGRESS_INTERVAL_SECS=",
                "SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE=",
                "SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED=0",
                "# model",
                "SIM_UAPI_W4_CHIPBACKEND_PROFILE=",
                "SIM_DEEPSEEK_V4_FLASH=",
                "SIM_W5_FLASH_WEIGHT_CATALOG=",
                "# serving",
                "SIM_LLM_INFER_PROMPT=",
                "SIM_LLM_INFER_PROMPT_TOKEN_IDS=",
                "SIM_W5_SERVING_REQUESTS_FILE=",
                "SIM_W5_SERVING_QUEUE=0",
                "SIM_W5_SERVING_INGRESS=cluster",
                "SIM_W5_SERVING_SUBMIT_REQUESTS_FILE=",
                "# test-memory-reuse",
                "SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE=0",
                "SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP=1",
                "SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=1",
                "SIM_W5_TEST_MEMORY_GSVA_KV=1",
                "SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE=1",
                "SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP=1",
                "SIM_W5_TEST_MEMORY_OBSERVATION_STORE=/tmp/w5-memory-store.json",
                "SIM_W5_TEST_VALIDATE_ONLY=",
                "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=",
                "SIM_W5_TEST_MEMORY_REUSE_DISABLE=0",
                "SIM_W5_TEST_MEMORY_REUSE_OUT_DIR=",
                "SIM_W5_TEST_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                "SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE=/tmp/w5-object-store.json",
                "SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=",
                "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                "SIM_W5_TEST_REQUIRE_PREFIX_CACHE=0",
                "# test-maintenance",
                "SIM_W5_TEST_POST_RUN_PRUNE=",
                "SIM_W5_TEST_POST_RUN_HEALTH=",
                "SIM_W5_TEST_ARTIFACT_KEEP_LATEST=3",
                "SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES=0",
                "SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES=0",
                "# vendor-context-test",
                "SIM_ENGRAM_SIMT_ARTIFACT_DIR=/tmp/engram-simt",
                "SIM_ENGRAM_SIMT_SELECTED_SYMBOL=engram_context_dim8_b1",
                "SIM_ENGRAM_SIMT_SELECTED_CASE=dim8_batch1",
                "SIM_ENGRAM_SIMT_BINARY_PATH=/tmp/engram-simt/engram-simt",
                "SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH=/tmp/engram-simt/libkernel.so",
            ],
        )

    def test_w5_cluster_config_runner_requires_explicit_config(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        result = subprocess.run(
            [str(config_runner), "--print-env"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("W5 cluster config file is required", result.stderr)
        self.assertIn("run_w5_cluster_qwen3_0_6b_2step.sh", result.stderr)

    def test_w5_cluster_config_runner_accepts_serving_requests_file(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            requests_path = Path(tmp) / "requests.txt"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            requests_path.write_text(
                "\n".join(
                    [
                        "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=2",
                        "request_id=req-b prompt_token_ids=81378,37585,374,17 decode_steps=3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(config_runner),
                    "--print-env",
                    "--requests",
                    str(requests_path),
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(f"SIM_W5_SERVING_REQUESTS_FILE={requests_path}", result.stdout)

    def test_w5_cluster_config_runner_accepts_serving_queue_mode(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(config_runner),
                    "--print-env",
                    "--serve-queue",
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_W5_SERVING_QUEUE=1", result.stdout)

    def test_w5_cluster_config_runner_accepts_runtime_serving_requests(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            requests_path = Path(tmp) / "serve-requests.txt"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            requests_path.write_text(
                "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=1\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(config_runner),
                    "--print-env",
                    "--serve-requests",
                    str(requests_path),
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_W5_SERVING_QUEUE=1", result.stdout)
        self.assertIn(f"SIM_W5_SERVING_SUBMIT_REQUESTS_FILE={requests_path}", result.stdout)
        self.assertIn("SIM_W5_SERVING_REQUESTS_FILE=\n", result.stdout)

    def test_w5_cluster_config_runner_accepts_nodea_ingress_for_dynamic_request(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            requests_path = Path(tmp) / "serve-requests.txt"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_LLM_INFER_PROMPT_TOKEN_IDS=81378,37585,374",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=1",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            requests_path.write_text(
                "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=1\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(config_runner),
                    "--print-env",
                    "--nodea-ingress",
                    "--serve-requests",
                    str(requests_path),
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_W5_SERVING_QUEUE=1", result.stdout)
        self.assertIn("SIM_W5_SERVING_INGRESS=nodeA", result.stdout)
        self.assertIn(f"SIM_W5_SERVING_SUBMIT_REQUESTS_FILE={requests_path}", result.stdout)

    def test_w5_cluster_config_runner_accepts_nodea_ingress_multi_request(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            requests_path = Path(tmp) / "serve-requests.txt"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_LLM_INFER_PROMPT_TOKEN_IDS=81378,37585,374",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=1",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            requests_path.write_text(
                "\n".join(
                    [
                        "request_id=req-a prompt_token_ids=81378,37585,999 decode_steps=1",
                        "request_id=req-b prompt_token_ids=81378,37585,374 decode_steps=1",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(config_runner),
                    "--print-env",
                    "--nodea-ingress",
                    "--serve-requests",
                    str(requests_path),
                    str(config_path),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 0)
        self.assertIn("SIM_W5_SERVING_QUEUE=1", result.stdout)
        self.assertIn("SIM_W5_SERVING_INGRESS=nodeA", result.stdout)
        self.assertIn(f"SIM_W5_SERVING_SUBMIT_REQUESTS_FILE={requests_path}", result.stdout)

    def test_w5_nodea_ingress_scopes_each_request_decode_step_base(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        legacy_runner = (script_dir / "run_llm_infer_eight_node_guest.sh").read_text(
            encoding="utf-8"
        )

        self.assertIn('SIM_W5_SERVING_DECODE_STEP_BASE="\\$request_step_base"', legacy_runner)
        self.assertIn("export SIM_W5_SERVING_DECODE_STEP_BASE", legacy_runner)
        self.assertIn(
            "request_step_base=\\$((request_step_base + SIM_QWEN3_GUEST_DECODE_STEPS))",
            legacy_runner,
        )

    def test_w5_prefix_cache_realistic_matrix_runner_dry_run(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        matrix_runner = script_dir / "run_w5_prefix_cache_realistic_matrix.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=4",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3-0.6b",
                        "SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP=1",
                        "SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=1",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(matrix_runner),
                    "--dry-run",
                    "--steps",
                    "8",
                    "--reuse-runs",
                    "2",
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("=== W5 Prefix Cache Realistic Matrix ===", result.stdout)
        self.assertIn("Steps:      8", result.stdout)
        self.assertIn("Reuse runs: 2", result.stdout)
        self.assertEqual(result.stdout.count("--no-memory-reuse"), 1)
        self.assertEqual(result.stdout.count("--require-prefix-cache"), 2)
        self.assertEqual(result.stdout.count("--compare-prefix-cache-benefit"), 2)
        self.assertIn("run_w5_cluster_config.sh", result.stdout)
        self.assertIn("w5_inference_run_report.py", result.stdout)

    def test_w5_prefix_cache_realistic_matrix_runner_rejects_invalid_args(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        matrix_runner = script_dir / "run_w5_prefix_cache_realistic_matrix.sh"

        result = subprocess.run(
            [str(matrix_runner), "--dry-run", "--steps", "0"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--steps must be a positive integer: 0", result.stderr)

    def test_w5_prefix_cache_serving_matrix_runner_dry_run(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        matrix_runner = script_dir / "run_w5_prefix_cache_serving_matrix.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=4",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3-0.6b",
                        "SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP=1",
                        "SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=1",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(matrix_runner),
                    "--dry-run",
                    "--steps",
                    "8",
                    "--same-prefix-runs",
                    "2",
                    "--shared-prefix-token-ids",
                    "10,11,12",
                    "--suffix-a-token-ids",
                    "13,15,17",
                    "--suffix-b-token-ids",
                    "14,16,18",
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("=== W5 Prefix Cache Serving Matrix ===", result.stdout)
        self.assertIn("Steps:            8", result.stdout)
        self.assertIn("Same-prefix runs: 2", result.stdout)
        self.assertIn("Shared prefix:    10,11,12", result.stdout)
        self.assertIn("Shared tokens:    3", result.stdout)
        self.assertIn("Suffix A replay:  2", result.stdout)
        self.assertIn("Suffix B replay:  2", result.stdout)
        self.assertIn("Prompt A:         10,11,12,13,15,17", result.stdout)
        self.assertIn("Prompt B:         10,11,12,14,16,18", result.stdout)
        self.assertEqual(result.stdout.count("--no-memory-reuse"), 1)
        self.assertEqual(result.stdout.count("--require-prefix-cache"), 9)
        self.assertEqual(result.stdout.count("--expect-prefix-cache-matched-tokens"), 4)
        self.assertEqual(result.stdout.count("--expect-prefix-cache-suffix-replay-tokens"), 4)
        self.assertEqual(result.stdout.count("--compare-prefix-cache-benefit"), 4)
        self.assertEqual(result.stdout.count("--compare-prefix-cache "), 0)
        self.assertIn("benefit_comparisons=4", result.stdout)
        self.assertIn("run=seed-shared-prefix", result.stdout)
        self.assertIn("run=reuse-request-b-1", result.stdout)
        self.assertIn("run=reuse-request-b-2", result.stdout)
        self.assertIn("expect-fail", result.stdout)
        self.assertIn("run_w5_cluster_config.sh", result.stdout)
        self.assertIn("w5_inference_run_report.py", result.stdout)

    def test_w5_prefix_cache_serving_matrix_runner_rejects_invalid_args(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        matrix_runner = script_dir / "run_w5_prefix_cache_serving_matrix.sh"

        result = subprocess.run(
            [str(matrix_runner), "--dry-run", "--suffix-a-token-ids", "13", "--suffix-b-token-ids", "13"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--suffix-a-token-ids and --suffix-b-token-ids must differ",
            result.stderr,
        )
    def test_w5_cluster_config_runner_prints_post_run_maintenance_flags(self):
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
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    str(config_runner),
                    "--print-env",
                    "--post-run-prune",
                    "--keep-latest",
                    "4",
                    str(config_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_W5_TEST_POST_RUN_PRUNE=1", result.stdout)
        self.assertIn("SIM_W5_TEST_POST_RUN_HEALTH=1", result.stdout)
        self.assertIn("SIM_W5_TEST_ARTIFACT_KEEP_LATEST=4", result.stdout)

    def test_w5_cluster_config_runner_rejects_vendor_context_guard_without_fused_simt(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context requires "
            "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
            result.stderr,
        )

    def test_w5_cluster_config_runner_rejects_vendor_context_guard_without_guest_engram(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            artifact_dir = tmp_path / "engram-simt-build"
            artifact_dir.mkdir()
            (artifact_dir / "engram-simt").write_bytes(b"binary")
            (artifact_dir / "libengram-simt_kernel.so").write_bytes(b"kernel")
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                        f"SIM_ENGRAM_SIMT_ARTIFACT_DIR={artifact_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context requires "
            "SIM_QWEN3_GUEST_ENGRAM=1",
            result.stderr,
        )

    def test_w5_cluster_config_runner_rejects_vendor_context_guard_without_artifact(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "requires SIM_ENGRAM_SIMT_ARTIFACT_DIR or complete "
            "SIM_ENGRAM_SIMT_SELECTED_* vendor env",
            result.stderr,
        )

    def test_w5_cluster_config_runner_rejects_incomplete_vendor_artifact_dir(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            artifact_dir = tmp_path / "engram-simt-build"
            artifact_dir.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                        f"SIM_ENGRAM_SIMT_ARTIFACT_DIR={artifact_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            f"SIM_ENGRAM_SIMT_ARTIFACT_DIR is missing engram-simt: {artifact_dir}",
            result.stderr,
        )

    def test_w5_cluster_config_runner_accepts_complete_vendor_artifact_dir(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            artifact_dir = tmp_path / "engram-simt-build"
            artifact_dir.mkdir()
            (artifact_dir / "engram-simt").write_bytes(b"binary")
            (artifact_dir / "libengram-simt_kernel.so").write_bytes(b"kernel")
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                        f"SIM_ENGRAM_SIMT_ARTIFACT_DIR={artifact_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_w5_cluster_config_runner_rejects_missing_selected_vendor_binary_path(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            kernel_path = tmp_path / "libengram_simt.so"
            kernel_path.write_bytes(b"kernel")
            missing_binary_path = tmp_path / "engram_simt_missing"
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                        "SIM_ENGRAM_SIMT_SELECTED_SYMBOL=engram_context_dim8_b1",
                        "SIM_ENGRAM_SIMT_SELECTED_CASE=dim8_batch1",
                        f"SIM_ENGRAM_SIMT_BINARY_PATH={missing_binary_path}",
                        f"SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH={kernel_path}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            f"SIM_ENGRAM_SIMT_BINARY_PATH is missing: {missing_binary_path}",
            result.stderr,
        )

    def test_w5_cluster_config_runner_accepts_selected_vendor_paths(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            binary_path = tmp_path / "engram_simt"
            kernel_path = tmp_path / "libengram_simt.so"
            binary_path.write_bytes(b"binary")
            kernel_path.write_bytes(b"kernel")
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context",
                        "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt",
                        "SIM_ENGRAM_SIMT_SELECTED_SYMBOL=engram_context_dim8_b1",
                        "SIM_ENGRAM_SIMT_SELECTED_CASE=dim8_batch1",
                        f"SIM_ENGRAM_SIMT_BINARY_PATH={binary_path}",
                        f"SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH={kernel_path}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--validate-only", str(config_path)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 0, result.stderr)

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
                if run_id != failed_run:
                    self.write_w5_reuse_summary(out_dir, run_id, steps=2)
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
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{new_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={out_dir}/w5_object_service_store.{new_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={new_run}", result.stdout)

    def test_w5_cluster_config_runner_prefers_runtime_boundary_store_for_latest_reuse(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_memory_object_store.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            self.write_w5_reuse_summary(out_dir, run_id, steps=2)
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{run_id}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={out_dir}/w5_object_service_store.{run_id}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={run_id}", result.stdout)

    def test_w5_cluster_config_runner_skips_latest_reuse_run_with_incomplete_boundary_coverage(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            complete_run = "2026-05-26_01-00-00_w5_qwen3_14b_engram_decode_111"
            incomplete_run = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            for index, run_id in enumerate([complete_run, incomplete_run], start=1):
                decision_store = out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json"
                object_store = out_dir / f"w5_object_service_store.{run_id}.json"
                decision_store.write_text("{}", encoding="utf-8")
                object_store.write_text("{}", encoding="utf-8")
                missing_boundary = (1, 7) if run_id == incomplete_run else None
                self.write_w5_reuse_summary(out_dir, run_id, steps=2, missing_boundary=missing_boundary)
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
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{complete_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={complete_run}", result.stdout)

    def test_w5_cluster_config_runner_skips_latest_boundary_only_reuse_run(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            executable_run = "2026-05-26_01-00-00_w5_qwen3_14b_decode_111"
            boundary_only_run = "2026-05-26_02-00-00_w5_qwen3_14b_decode_222"
            executable_store = out_dir / f"w5_memory_object_store.{executable_run}.json"
            boundary_store = out_dir / f"w5_memory_runtime_boundary_lookup.{boundary_only_run}.json"
            executable_store.write_text("{}", encoding="utf-8")
            boundary_store.write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{executable_run}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{boundary_only_run}.json").write_text("{}", encoding="utf-8")
            self.write_w5_reuse_summary(out_dir, executable_run, steps=2)
            self.write_w5_boundary_only_summary(out_dir, boundary_only_run, steps=2)
            os.utime(executable_store, (1700000000, 1700000000))
            os.utime(boundary_store, (1700000001, 1700000001))
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_DENSE_WEIGHTS_PATH=/tmp/qwen3-14b",
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_object_store.{executable_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={executable_run}", result.stdout)

    def test_w5_cluster_config_runner_skips_latest_shortpath_only_reuse_run(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            complete_run = "2026-05-26_01-00-00_w5_qwen3_14b_engram_decode_111"
            shortpath_only_run = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            for index, run_id in enumerate([complete_run, shortpath_only_run], start=1):
                decision_store = out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json"
                object_store = out_dir / f"w5_object_service_store.{run_id}.json"
                decision_store.write_text("{}", encoding="utf-8")
                object_store.write_text("{}", encoding="utf-8")
                if run_id == shortpath_only_run:
                    self.write_w5_shortpath_only_summary(out_dir, run_id, steps=2)
                else:
                    self.write_w5_reuse_summary(out_dir, run_id, steps=2)
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
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{complete_run}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={complete_run}", result.stdout)

    def test_w5_cluster_config_runner_auto_reuse_miss_leaves_reuse_unset(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=", result.stdout)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_STORE=", result.stdout)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE=", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_w5_cluster_config_runner_require_prefix_cache_rejects_auto_reuse_miss(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
                        "SIM_W5_TEST_REQUIRE_PREFIX_CACHE=1",
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
        self.assertIn(
            "SIM_W5_TEST_REQUIRE_PREFIX_CACHE requires a reusable Memory Service decision store",
            result.stderr,
        )

    def test_w5_cluster_config_runner_require_prefix_cache_accepts_boundary_seed_candidate(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            self.write_w5_boundary_only_summary(out_dir, run_id, steps=2)
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
                        "SIM_W5_TEST_REQUIRE_PREFIX_CACHE=1",
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

        self.assertIn(
            f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{run_id}.json",
            result.stdout,
        )
        self.assertIn(
            f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={out_dir}/w5_object_service_store.{run_id}.json",
            result.stdout,
        )
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={run_id}", result.stdout)

    def test_w5_cluster_config_runner_auto_reuse_resolves_latest_memory_store(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_memory_object_store.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            self.write_w5_reuse_summary(out_dir, run_id, steps=2)
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{run_id}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={out_dir}/w5_object_service_store.{run_id}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={run_id}", result.stdout)

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
            self.write_w5_reuse_summary(out_dir, run_id, steps=2)
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG={run_id}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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

        self.assertIn(f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{run_id}.json", result.stdout)
        self.assertIn(f"SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID={run_id}", result.stdout)

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
                        f"SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG={run_id}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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
        self.assertIn("reuse summary is missing completion/coverage evidence", result.stderr)

    def test_w5_cluster_config_runner_rejects_named_reuse_run_with_incomplete_boundary_coverage(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_engram_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            self.write_w5_reuse_summary(out_dir, run_id, steps=2, missing_boundary=(1, 7))
            weights_path = tmp_path / "qwen3-14b"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG={run_id}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
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
        self.assertIn("missing boundary observation for step=1 node=node7", result.stderr)

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
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest",
                        "SIM_W5_TEST_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
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
        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG cannot be combined", result.stderr)

    def test_w5_cluster_config_runner_rejects_legacy_memory_reuse_run_id_name(self):
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
                        "SIM_W5_TEST_MEMORY_REUSE_RUN_ID=latest",
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
        self.assertIn(
            "SIM_W5_TEST_MEMORY_REUSE_RUN_ID was renamed to SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG",
            result.stderr,
        )

    def test_w5_cluster_config_runner_uses_app_wait_secs_name(self):
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
                        "APP_WAIT_SECS=900",
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

        self.assertEqual(result.returncode, 0)
        self.assertNotIn("DEMO_WAIT_SECS", result.stdout)
        self.assertNotIn("DEMO_WAIT_SECS", result.stderr)

    def test_w5_inference_cluster_runner_does_not_reference_legacy_demo_wait_secs_name(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        runner = script_dir / "run_w5_inference_cluster_runtime.sh"
        runner_text = runner.read_text(encoding="utf-8")

        self.assertNotIn("DEMO_WAIT_SECS", runner_text)

    def test_w5_cluster_config_runner_rejects_fixed_run_id_for_real_runs(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "RUN_ID=test-run",
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
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

    def test_w5_cluster_config_runner_validate_only_accepts_basic_config_without_memory_path(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3"
            self.write_qwen3_14b_stub_weights(weights_path)
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
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

        self.assertIn("validate_only=1", result.stderr)
        self.assertIn("runtime_boundary_lookup=1", result.stderr)
        self.assertIn("validate_only: true", result.stdout)

    def test_w5_cluster_config_runner_validate_only_rejects_invalid_memory_decision_store(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            decision_store = tmp_path / "w5-decision-store.json"
            object_store = tmp_path / "w5-object-store.json"
            decision_store.write_text("{}", encoding="utf-8")
            object_store.write_text("{}", encoding="utf-8")
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_DECISION_STORE={decision_store}",
                        f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={object_store}",
                        "SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=bad-run",
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

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("load W5 execution decisions", result.stderr)

    def test_w5_cluster_config_runner_validate_only_rejects_missing_memory_decision_store(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            decision_store = tmp_path / "missing-w5-decision-store.json"
            object_store = tmp_path / "w5-object-store.json"
            object_store.write_text("{}", encoding="utf-8")
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_DECISION_STORE={decision_store}",
                        f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={object_store}",
                        "SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=missing-run",
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
        self.assertIn("Memory Service decision store is missing", result.stderr)

    def test_w5_cluster_config_runner_validate_only_rejects_missing_memory_decision_object_store(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            weights_path = tmp_path / "qwen3-14b"
            self.write_qwen3_14b_stub_weights(weights_path)
            decision_store = tmp_path / "w5-decision-store.json"
            object_store = tmp_path / "missing-w5-object-store.json"
            decision_store.write_text("{}", encoding="utf-8")
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_engram_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_DECISION_STORE={decision_store}",
                        f"SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE={object_store}",
                        "SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=missing-run",
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
        self.assertIn("Memory Service decision object store is missing", result.stderr)

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

    def test_w5_cluster_config_runner_validate_only_rejects_require_prefix_cache_without_prefix_cache_lookup(self):
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
                        "SIM_W5_TEST_REQUIRE_PREFIX_CACHE=1",
                        "SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=0",
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
        self.assertIn("SIM_W5_TEST_REQUIRE_PREFIX_CACHE requires SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=1", result.stderr)

    def test_w5_cluster_config_runner_require_prefix_cache_cli_sets_env(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            self.write_w5_boundary_only_summary(out_dir, run_id, steps=2)
            weights_path = tmp_path / "qwen3"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", "--require-prefix-cache", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_W5_TEST_REQUIRE_PREFIX_CACHE=1", result.stdout)
        self.assertIn(
            f"SIM_W5_TEST_MEMORY_DECISION_STORE={out_dir}/w5_memory_runtime_boundary_lookup.{run_id}.json",
            result.stdout,
        )

    def test_w5_cluster_config_runner_no_memory_reuse_cli_leaves_reuse_unset(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        config_runner = script_dir / "run_w5_cluster_config.sh"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            out_dir = tmp_path / "out"
            out_dir.mkdir()
            run_id = "2026-05-26_02-00-00_w5_qwen3_14b_decode_222"
            (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text("{}", encoding="utf-8")
            (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
            self.write_w5_boundary_only_summary(out_dir, run_id, steps=2)
            weights_path = tmp_path / "qwen3"
            weights_path.mkdir()
            config_path = tmp_path / "w5.env"
            config_path.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_14b_decode",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights_path}",
                        f"SIM_W5_TEST_MEMORY_REUSE_OUT_DIR={out_dir}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(config_runner), "--print-env", "--no-memory-reuse", str(config_path)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("SIM_W5_TEST_MEMORY_REUSE_DISABLE=1", result.stdout)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_STORE=", result.stdout)
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE=", result.stdout)
        self.assertIn("SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=", result.stdout)

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
                        "SIM_W5_TEST_MEMORY_DECISION_STORE=/tmp/w5-decision-store.json",
                        "SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=0",
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
        self.assertIn("SIM_W5_TEST_MEMORY_DECISION_STORE requires a boundary observation/decision selector", result.stderr)

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

    def test_w5_cluster_config_runner_rejects_invalid_keep_latest_override(self):
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
                [str(config_runner), "--validate-only", "--keep-latest", "bad", str(config_path)],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--keep-latest must be a non-negative integer: bad", result.stderr)


if __name__ == "__main__":
    unittest.main()
