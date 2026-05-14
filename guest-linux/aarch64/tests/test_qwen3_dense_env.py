#!/usr/bin/env python3
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


class Qwen3DenseEnvTest(unittest.TestCase):
    def run_env_probe(self, config):
        common = Path(__file__).resolve().parents[1] / "scripts" / "qemu_ub_common.sh"
        with tempfile.TemporaryDirectory() as tmp:
            model_dir = Path(tmp)
            (model_dir / "config.json").write_text(json.dumps(config), encoding="utf-8")
            (model_dir / "tokenizer.json").write_text("{}", encoding="utf-8")
            (model_dir / "model.safetensors.index.json").write_text("{}", encoding="utf-8")

            probe = (
                "source \"$1\"\n"
                "SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_dense_0_6b\n"
                "SIM_QWEN3_DENSE_WEIGHTS_PATH=\"$2\"\n"
                "qwen3_dense_apply_config_env\n"
                "printf '%s\\n' \"$SIM_UAPI_W4_CHIPBACKEND_PROFILE\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_MODEL_KEY\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES\"\n"
                "printf '%s\\n' \"$SIM_QWEN3_DENSE_KV_STATE_BYTES\"\n"
            )
            result = subprocess.run(
                ["zsh", "-c", probe, "zsh", str(common), str(model_dir)],
                check=True,
                capture_output=True,
                text=True,
            )
            return result.stdout.strip().splitlines()

    def test_14b_config_switches_to_generic_profile_and_exports_dimensions(self):
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

        self.assertEqual(values, ["qwen3_dense", "qwen3-14b", "40", "1310720", "327680"])

    def test_0_6b_config_keeps_legacy_profile(self):
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

        self.assertEqual(values, ["qwen3_dense_0_6b", "qwen3-0-6b", "28", "262144", "229376"])

    def test_qwen3_0_6b_two_step_wrapper_has_stable_defaults(self):
        script_dir = Path(__file__).resolve().parents[1] / "scripts"
        wrapper = script_dir / "run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh"

        self.assertTrue(wrapper.exists())
        self.assertTrue(wrapper.stat().st_mode & 0o111)

        text = wrapper.read_text(encoding="utf-8")
        self.assertIn("SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense_0_6b", text)
        self.assertIn("SIM_QWEN3_GUEST_DECODE_STEPS:-2", text)
        self.assertIn("/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B", text)
        self.assertIn('exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"', text)


if __name__ == "__main__":
    unittest.main()
