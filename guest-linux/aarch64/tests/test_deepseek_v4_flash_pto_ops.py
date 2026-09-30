import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux" / "aarch64" / "libs" / "lingqu_shmem_pto"


class DeepseekV4FlashPtoOpsTest(unittest.TestCase):
    def _check_quantized_golden(self, kind):
        compiler = os.environ.get("CXX") or shutil.which("g++-15") or "c++"
        source = (
            ROOT / "guest-linux" / "aarch64" / "tests"
            / f"deepseek_v4_flash_{kind}_pto_golden.cpp"
        )
        version = subprocess.run(
            [compiler, "--version"], check=True, capture_output=True, text=True
        ).stdout
        with tempfile.TemporaryDirectory() as temporary:
            for name, flags in (
                ("regular", ["-O2"]),
                ("production", ["-O3", "-ffast-math", "-fno-finite-math-only"]),
            ):
                with self.subTest(optimization=name):
                    binary = Path(temporary) / f"deepseek_{kind}_{name}"
                    command = [
                        compiler, "-std=c++20", *flags,
                        "-Wall", "-Wextra", "-Werror", "-D__CPU_SIM", "-pthread",
                        "-isystem", str(ROOT / "vendor" / "pto-isa" / "include"),
                        "-I", str(LIB), str(source), "-o", str(binary),
                    ]
                    if "clang" in version.lower():
                        command.insert(command.index("-pthread"), "-Wno-pass-failed")
                    subprocess.run(command, check=True)
                    completed = subprocess.run(
                        [str(binary)], capture_output=True, text=True
                    )
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    self.assertEqual(
                        completed.stdout.strip(),
                        f"deepseek_v4_flash_{kind}_pto=pass",
                    )

    def test_q8k_quantization_uses_pto_and_rejects_nonfinite_input(self):
        self._check_quantized_golden("q8k")

    def test_fp4_indexer_roundtrip_matches_scalar_boundaries(self):
        self._check_quantized_golden("fp4")

    def test_fp8_kv_roundtrip_matches_scalar_boundaries(self):
        self._check_quantized_golden("fp8")

    def test_q8_projection_executes_with_pto_load_store_and_math(self):
        compiler = os.environ.get("CXX") or shutil.which("g++-15") or "c++"
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "deepseek_v4_flash_pto_ops_golden"
            command = [
                    compiler,
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-D__CPU_SIM",
                    "-pthread",
                    "-isystem",
                    str(ROOT / "vendor" / "pto-isa" / "include"),
                    "-I",
                    str(LIB),
                    str(
                        ROOT
                        / "guest-linux"
                        / "aarch64"
                        / "tests"
                        / "deepseek_v4_flash_pto_ops_golden.cpp"
                    ),
                    "-o",
                    str(binary),
                ]
            version = subprocess.run(
                [compiler, "--version"], check=True, capture_output=True, text=True
            ).stdout
            if "clang" in version.lower():
                command.insert(6, "-Wno-pass-failed")
            subprocess.run(command, check=True)
            completed = subprocess.run(
                [str(binary)], capture_output=True, text=True
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
        fields = dict(
            token.split("=", 1) for token in completed.stdout.strip().split()
        )
        self.assertEqual(fields["deepseek_v4_flash_pto_ops"], "pass")
        self.assertEqual(fields["q8_linear"], "1")
        self.assertEqual(fields["f16_linear"], "1")
        self.assertEqual(fields["q2_k_linear"], "1")
        self.assertEqual(fields["iq2_xxs_linear"], "1")
        self.assertEqual(fields["rms_norm"], "1")
        self.assertEqual(fields["head_rms_norm"], "1")
        self.assertEqual(fields["rope_tail"], "1")
        self.assertEqual(fields["kv_write"], "1")
        self.assertEqual(fields["kv_gather"], "1")
        self.assertEqual(fields["sink_attention"], "1")
        self.assertEqual(fields["compressor_update"], "1")
        self.assertEqual(fields["indexer_qat"], "1")
        self.assertEqual(fields["sparse_indexer"], "2")
        self.assertEqual(fields["router"], "1")
        self.assertEqual(fields["routed_experts"], "1")
        self.assertEqual(fields["shared_expert"], "1")
        self.assertEqual(fields["hc_split"], "1")
        self.assertEqual(fields["hc_weighted_sum"], "1")
        self.assertEqual(fields["hc_post"], "1")
        self.assertEqual(fields["swiglu"], "1")
        self.assertGreater(int(fields["tload"]), 0)
        self.assertGreater(int(fields["tstore"]), 0)


if __name__ == "__main__":
    unittest.main()
