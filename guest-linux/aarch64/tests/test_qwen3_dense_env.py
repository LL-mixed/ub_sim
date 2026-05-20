#!/usr/bin/env python3
import json
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
        self.assertIn("SIM_UAPI_W5_PROFILE", launcher_text)
        self.assertIn("SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS", launcher_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_STATE_REF", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR", runner_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT", runner_text)
        self.assertIn("qwen3_engram_context_refs_configured", runner_text)
        self.assertIn("qwen3_engram_context_op_enabled", runner_text)
        self.assertIn("validate_qwen3_engram_context_refs", runner_text)
        self.assertIn("context op requires EngramStateObjectRef", runner_text)
        self.assertIn("component refs are not a real W5 entrypoint", runner_text)
        self.assertIn("qwen3_engram_state_object_ref", runner_text)
        self.assertIn("validate_w5_engram_context_summary", runner_text)
        self.assertIn("target=uapi_object_ref", runner_text)
        self.assertIn("modes=[^ ]*object-ref", runner_text)
        self.assertIn("SIM_QWEN3_GUEST_ENGRAM_STATE_REF", launcher_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR", launcher_text)
        self.assertIn("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT", launcher_text)
        self.assertIn("SIM_W5_RUN_ID", runner_text)
        self.assertIn("SIM_W5_RUN_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_STORE", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_DECISION_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_START", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_END", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_KIND", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_REF", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_START", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_END", runner_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_POSITION", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_PLAN_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_IDS", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_REFS", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM", runner_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF", runner_text)
        self.assertIn("SIM_W5_MEMORY_DECISION_STORE", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_DECISION_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_START", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_END", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_KIND", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_ARTIFACT_REF", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_START", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_END", launcher_text)
        self.assertIn("SIM_W5_MEMORY_SHORTPATH_PRODUCER_POSITION", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_PLAN_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_IDS", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFETCH_ARTIFACT_REFS", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM", launcher_text)
        self.assertIn("SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF", launcher_text)

    def test_guest_consumes_w5_prefix_cache_reuse_as_kv_object_ref(self):
        guest_source = (
            Path(__file__).resolve().parents[1] / "w4_guest_qemu_demo.c"
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
        self.assertIn("qwen3_w5_memory_terminal_logits_loaded", guest_source)

    def test_w5_inference_cluster_runner_delegates_to_legacy_compatible_runner(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        runner = script_dir / "run_ub_eight_node_w5_inference_cluster.sh"
        generic = script_dir / "run_ub_w5_inference_cluster.sh"
        summary = script_dir / "w5_inference_cluster_summary.py"

        self.assertTrue(runner.exists())
        self.assertTrue(runner.stat().st_mode & 0o111)
        self.assertTrue(generic.exists())
        self.assertTrue(generic.stat().st_mode & 0o111)
        self.assertTrue(summary.exists())
        self.assertTrue(summary.stat().st_mode & 0o111)

        runner_text = runner.read_text(encoding="utf-8")
        generic_text = generic.read_text(encoding="utf-8")
        summary_text = summary.read_text(encoding="utf-8")

        self.assertIn("SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode", runner_text)
        self.assertIn("eight_node_w5_inference_cluster_summary", runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"', runner_text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"', generic_text)
        self.assertIn("w4_guest_run_summary.py", summary_text)


if __name__ == "__main__":
    unittest.main()
