import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux" / "aarch64" / "libs" / "lingqu_shmem_pto"


class DeepseekV4FlashPtoRangeTest(unittest.TestCase):
    def test_direct_range_probe_compiles_and_sizes_state(self):
        compiler = os.environ.get("CXX", "c++")
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "deepseek_v4_flash_pto_range_probe"
            command = [
                compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                "-D__CPU_SIM", "-pthread", "-isystem",
                str(ROOT / "vendor" / "pto-isa" / "include"),
                "-I", str(LIB),
                str(ROOT / "guest-linux" / "aarch64" / "tests"
                    / "deepseek_v4_flash_pto_range_probe.cpp"),
                "-o", str(binary),
            ]
            version = subprocess.run(
                [compiler, "--version"], check=True, capture_output=True, text=True
            ).stdout
            if "clang" in version.lower():
                command.insert(6, "-Wno-pass-failed")
            subprocess.run(command, check=True)
            bias_name = subprocess.run(
                [str(binary), "--selection-bias-name"],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(bias_name.stdout.strip(), "blk.3.exp_probs_b.bias")
            for end, positions, expected in [
                (1, 1, 2096), (2, 1, 4160), (1, 2, 4144),
                (43, 2, 12385408),
            ]:
                result = subprocess.run(
                    [str(binary), "--state-bytes", str(end), str(positions)],
                    check=True, capture_output=True, text=True,
                )
                self.assertEqual(int(result.stdout.strip()), expected)
            fixture = subprocess.run(
                [str(binary), "--sink-attention-fixture"],
                check=True, capture_output=True, text=True,
            )
            self.assertIn("sink_attention_fixture=pass", fixture.stdout)
            outputs = [str(Path(temporary) / name) for name in
                       ("hidden.f32", "state.bin", "logits.f32")]
            missing_index = str(Path(temporary) / "missing-index")
            rejected = subprocess.run(
                [str(binary), missing_index, "1", "42", *outputs[:2],
                 "--logits-out", outputs[2]],
                capture_output=True, text=True,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("invalid_range_or_token", rejected.stderr)
            accepted = subprocess.run(
                [str(binary), missing_index, "43", "42", *outputs[:2],
                 "--logits-out", outputs[2]],
                capture_output=True, text=True,
            )
            self.assertNotEqual(accepted.returncode, 0)
            self.assertIn("weight_index_read_failed", accepted.stderr)
            sequence = subprocess.run(
                [str(binary), missing_index, "43", "42,201", *outputs[:2],
                 "--logits-out", outputs[2]],
                capture_output=True, text=True,
            )
            self.assertNotEqual(sequence.returncode, 0)
            self.assertIn("weight_index_read_failed", sequence.stderr)
            prefix = subprocess.run(
                [str(binary), missing_index, "43", "42,201", *outputs[:2],
                 "--logits-prefix", str(Path(temporary) / "step")],
                capture_output=True, text=True,
            )
            self.assertNotEqual(prefix.returncode, 0)
            self.assertIn("weight_index_read_failed", prefix.stderr)
            invalid_prefix = subprocess.run(
                [str(binary), missing_index, "1", "42", *outputs[:2],
                 "--logits-prefix", str(Path(temporary) / "step")],
                capture_output=True, text=True,
            )
            self.assertNotEqual(invalid_prefix.returncode, 0)
            self.assertIn("invalid_range_or_token", invalid_prefix.stderr)
            grouped = subprocess.run(
                [str(binary), missing_index, "--grouped-output-a",
                 outputs[0], outputs[2]],
                capture_output=True, text=True,
            )
            self.assertNotEqual(grouped.returncode, 0)
            self.assertIn("weight_index_read_failed", grouped.stderr)
            malformed = subprocess.run(
                [str(binary), missing_index, "43", "42,", *outputs[:2],
                 "--logits-out", outputs[2]],
                capture_output=True, text=True,
            )
            self.assertNotEqual(malformed.returncode, 0)
            self.assertIn("invalid_token_sequence", malformed.stderr)

    def test_range_header_compiles_with_strict_warnings(self):
        compiler = os.environ.get("CXX", "c++")
        source = '#include "deepseek_v4_flash_pto_range.hpp"\nint main() { return 0; }\n'
        command = [
            compiler,
            "-std=c++20",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D__CPU_SIM",
            "-pthread",
            "-isystem",
            str(ROOT / "vendor" / "pto-isa" / "include"),
            "-I",
            str(LIB),
            "-x",
            "c++",
            "-fsyntax-only",
            "-",
        ]
        version = subprocess.run(
            [compiler, "--version"], check=True, capture_output=True, text=True
        ).stdout
        if "clang" in version.lower():
            command.insert(6, "-Wno-pass-failed")
        subprocess.run(command, input=source, text=True, check=True)

    def test_range_graph_contains_complete_direct_model_stages(self):
        source = (LIB / "deepseek_v4_flash_pto_range.hpp").read_text()
        for stage in (
            "attention_layer(",
            "compressor_step(",
            "selected_compressed_rows(",
            "sink_attention(",
            "ffn_layer(",
            "routed_experts(",
            "shared_expert(",
            "output_head(",
            "decoder_range(",
        ):
            self.assertIn(stage, source)
        self.assertIn(
            "LocalMatrix output_a(1, kOutputGroups * kOLora)", source
        )
        self.assertNotIn("std::memcpy(", source)

    def test_artifact_profile_freezes_callable_five_contract(self):
        path = ROOT / "guest-linux/aarch64/scripts/prepare_simpler_host_artifacts.py"
        spec = importlib.util.spec_from_file_location(
            "deepseek_pto_range_artifact_contract", path
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        profile = module.PROFILE_SPECS["host_deepseek_v4_flash_range"]
        self.assertEqual(len(profile.args_template), 8)
        self.assertEqual(profile.args_template[4]["kind"], "inout")
        self.assertEqual(profile.args_template[-1]["name"], "private_geometry")
        self.assertEqual(
            profile.orch_function,
            "build_deepseek_v4_flash_pto_range_graph",
        )
        self.assertEqual(profile.kernels[0].source,
                         "deepseek_v4_flash_pto_range_kernel.cpp")
        self.assertEqual(
            module.resolve_example_root(Path("unused"), profile), LIB
        )

    def test_kernel_uses_private_weight_index_and_direct_decoder(self):
        kernel = (LIB / "deepseek_v4_flash_pto_range_kernel.cpp").read_text()
        orchestration = (
            LIB / "deepseek_v4_flash_pto_range_orch.cpp"
        ).read_text()
        self.assertIn("WeightIndex index", kernel)
        self.assertIn("MappedWeights weights", kernel)
        self.assertIn("decoder_range(", kernel)
        self.assertIn("deepseek_pto_kernel_error=%s", kernel)
        self.assertIn("expected_arg_count = 8", orchestration)
        self.assertIn("args.scalar_count() != 0", orchestration)
        self.assertIn("geometry_descriptor", kernel)
        self.assertIn("task.add_output(args.tensor(3).ref())", orchestration)
        self.assertIn("task.add_inout(args.tensor(4).ref())", orchestration)
        self.assertIn(
            "args.tensor(5).ref().nbytes() > sizeof(float)", orchestration
        )


if __name__ == "__main__":
    unittest.main()
