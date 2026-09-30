import importlib.util
import math
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux" / "aarch64" / "libs" / "lingqu_shmem_pto"


class DeepseekV4FlashPtoRangeTest(unittest.TestCase):
    def test_terminal_trace_mirrors_complete_logits_without_output_read(self):
        compiler = os.environ.get("CXX") or shutil.which("g++-15") or "c++"
        source = (
            '#include "deepseek_v4_flash_pto_trace.hpp"\n'
            '#include <array>\n'
            '#include <cstdint>\n'
            '#include <vector>\n'
            'int main(int argc, char **) {\n'
            '  namespace ds = lingqu::deepseek_v4_flash_pto;\n'
            '  ds::RangeGeometry geometry{};\n'
            '  geometry.first = 40;\n'
            '  geometry.end = argc == 1 ? ds::kLayers : ds::kLayers - 1;\n'
            '  geometry.past = 7;\n'
            '  geometry.tokens = 1;\n'
            '  ds::TerminalLogitsTrace trace(geometry);\n'
            '  if (!trace.enabled()) return 0;\n'
            '  const ds::F32View logits = trace.view();\n'
            '  const uint32_t samples[]{0, 42, ds::kVocab - 1};\n'
            '  for (uint32_t i : samples) {\n'
            '    ds::F32Scalar value;\n'
            '    pto::TEXPANDS(value, float(i) * 0.125f);\n'
            '    auto destination = ds::global_row<float>(logits.at(0, i), 1);\n'
            '    pto::TSTORE(destination, value);\n'
            '  }\n'
            '  std::array<float, 32> input;\n'
            '  input.fill(1.0f);\n'
            '  std::array<uint8_t, 34> encoded{};\n'
            '  encoded[1] = 0x3c;\n'
            '  for (uint32_t i = 2; i < encoded.size(); ++i) encoded[i] = 1;\n'
            '  ds::WeightTensor weight{};\n'
            '  weight.payload = encoded.data();\n'
            '  weight.bytes = encoded.size();\n'
            '  weight.dimensions[0] = input.size();\n'
            '  weight.dimensions[1] = 1;\n'
            '  weight.tensor_type = ds::kGgufTypeQ8_0;\n'
            '  weight.rank = 2;\n'
            '  float output = 0.0f;\n'
            '  ds::F32View source{reinterpret_cast<uintptr_t>(input.data()),\n'
            '                     1, 32, 32, 1};\n'
            '  ds::F32View destination{reinterpret_cast<uintptr_t>(&output),\n'
            '                          1, 1, 1, 1};\n'
            '  ds::F32View mirror{logits.at(0, 42), 1, 1, 1, 1};\n'
            '  ds::q8_0_linear(source, weight, destination, &mirror);\n'
            '  if (output != *reinterpret_cast<float *>(mirror.address))\n'
            '    return 2;\n'
            '  trace.finish();\n'
            '  return 0;\n'
            '}\n'
        )
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "deepseek-pto-trace-fixture"
            command = [
                compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                "-D__CPU_SIM", "-pthread", "-isystem",
                str(ROOT / "vendor" / "pto-isa" / "include"),
                "-I", str(LIB), "-x", "c++", "-", "-o", str(binary),
            ]
            version = subprocess.run(
                [compiler, "--version"], check=True, capture_output=True,
                text=True,
            ).stdout
            if "clang" in version.lower():
                command.insert(6, "-Wno-pass-failed")
            subprocess.run(command, input=source, text=True, check=True)
            environment = os.environ.copy()
            environment["SIM_W5_TEST_PTO_TRACE_DIR"] = temporary
            subprocess.run([str(binary), "nonterminal"], env=environment,
                           check=True)
            self.assertEqual(list(Path(temporary).glob("*.f32")), [])
            subprocess.run([str(binary)], env=environment, check=True)
            output = (
                Path(temporary) /
                "deepseek-range-40-43-past-7-tokens-1-logits.f32"
            )
            data = output.read_bytes()
            self.assertEqual(len(data), 129280 * 4)
            self.assertEqual(struct.unpack_from("<f", data, 0)[0], 0.0)
            self.assertEqual(struct.unpack_from("<f", data, 129279 * 4)[0],
                             129279 * 0.125)
            self.assertGreater(struct.unpack_from("<f", data, 42 * 4)[0],
                               0.0)
            duplicate = subprocess.run([str(binary)], env=environment,
                                       capture_output=True, text=True)
            self.assertNotEqual(duplicate.returncode, 0)
            self.assertIn("deepseek_pto_trace_open_failed", duplicate.stderr)

    def test_direct_range_probe_compiles_and_sizes_state(self):
        compiler = os.environ.get("CXX") or shutil.which("g++-15") or "c++"
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
            before = subprocess.run(
                [str(binary), "--state-bytes", "3", "2051"],
                check=True, capture_output=True, text=True,
            )
            after = subprocess.run(
                [str(binary), "--state-bytes", "3", "2052"],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(int(after.stdout) - int(before.stdout), 2560)
            invalid_seed = subprocess.run(
                [str(binary), "missing.index", "3", "42",
                 str(Path(temporary) / "hidden.f32"),
                 str(Path(temporary) / "state.bin"), "--seeded-past", "2050"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(invalid_seed.returncode, 0)
            self.assertIn("invalid_range_or_token", invalid_seed.stderr)
            fixture = subprocess.run(
                [str(binary), "--sink-attention-fixture"],
                check=True, capture_output=True, text=True,
            )
            self.assertIn("sink_attention_fixture=pass", fixture.stdout)
            token_fixture = subprocess.run(
                [str(binary), "--token-control-fixture"],
                check=True, capture_output=True, text=True,
            )
            self.assertIn(
                "token_control_fixture=pass tload=2 tstore=0",
                token_fixture.stdout,
            )
            q_input = Path(temporary) / "q.f32"
            kv_input = Path(temporary) / "kv.f32"
            sinks_input = Path(temporary) / "sinks.f32"
            attention_heads = Path(temporary) / "attention-heads.f32"
            q_values = bytearray(64 * 512 * 4)
            kv_values = bytearray(512 * 4)
            struct.pack_into("<f", q_values, 0, 1.0)
            struct.pack_into("<f", kv_values, 0, 1.0)
            q_input.write_bytes(q_values)
            kv_input.write_bytes(kv_values)
            sinks_input.write_bytes(bytes(64 * 4))
            subprocess.run(
                [str(binary), "--attention-heads-from-qkv",
                 str(q_input), str(kv_input), str(sinks_input), "0",
                 str(attention_heads)],
                check=True, capture_output=True, text=True,
            )
            heads = attention_heads.read_bytes()
            self.assertEqual(len(heads), 64 * 512 * 4)
            score = 1.0 / math.sqrt(512)
            expected = math.exp(score) / (1.0 + math.exp(score))
            self.assertAlmostEqual(struct.unpack_from("<f", heads, 0)[0],
                                   expected, places=5)
            self.assertAlmostEqual(struct.unpack_from("<f", heads, 512 * 4)[0],
                                   0.5, places=5)
            invalid_attention = subprocess.run(
                [str(binary), "--attention-heads-from-qkv",
                 str(q_input), str(sinks_input), str(sinks_input), "0",
                 str(attention_heads)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(invalid_attention.returncode, 0)
            self.assertIn("invalid_attention_input_geometry",
                          invalid_attention.stderr)
            q8_input = Path(temporary) / "boundary.f32"
            boundary = [0.0] * 64
            boundary[0], boundary[27] = 1.04024875, 0.520124376
            boundary[32], boundary[59] = 1.04024887, 0.520124435
            q8_input.write_bytes(struct.pack("<64f", *boundary))
            for block, expected in ((0, "63"), (1, "64")):
                result = subprocess.run(
                    [str(binary), "--q8-code", str(q8_input), str(block), "27"],
                    check=True, capture_output=True, text=True,
                )
                self.assertIn(f"code={expected}", result.stdout)
            invalid_q8 = subprocess.run(
                [str(binary), "--q8-code", str(q8_input), "2", "27"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(invalid_q8.returncode, 0)
            self.assertIn("invalid_q8_probe_geometry", invalid_q8.stderr)
            ds4_heads = Path(temporary) / "ds4-heads.f32"
            pto_heads = Path(temporary) / "pto-heads.f32"
            ds4_heads.write_bytes(struct.pack("<32f", *boundary[:32]))
            pto_heads.write_bytes(struct.pack("<32f", *boundary[32:]))
            diff = subprocess.run(
                [str(binary), "--q8-diff", str(ds4_heads), str(pto_heads)],
                check=True, capture_output=True, text=True,
            )
            self.assertIn("first_code_block=0 lane=27", diff.stdout)
            self.assertIn("code_differences=1", diff.stdout)
            identical = subprocess.run(
                [str(binary), "--q8-diff", str(ds4_heads), str(ds4_heads)],
                check=True, capture_output=True, text=True,
            )
            self.assertIn("code_differences=0", identical.stdout)
            q8_codes = Path(temporary) / "codes.bin"
            q8_scales = Path(temporary) / "scales.f32"
            dumped = subprocess.run(
                [str(binary), "--q8-dump", str(q8_input),
                 str(q8_codes), str(q8_scales)],
                check=True, capture_output=True, text=True,
            )
            self.assertIn("q8_blocks=2 dump=pass", dumped.stdout)
            self.assertEqual(q8_codes.stat().st_size, 64)
            self.assertEqual(q8_scales.stat().st_size, 8)
            self.assertEqual(q8_codes.read_bytes()[27], 63)
            self.assertEqual(q8_codes.read_bytes()[59], 64)
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
        compiler = os.environ.get("CXX") or shutil.which("g++-15") or "c++"
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
