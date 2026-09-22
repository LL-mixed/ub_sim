import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux" / "aarch64" / "libs" / "lingqu_shmem_pto"


class DeepseekV4FlashPtoOpsTest(unittest.TestCase):
    def test_q8_projection_executes_with_pto_load_store_and_math(self):
        compiler = os.environ.get("CXX", "c++")
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
                [str(binary)], check=True, capture_output=True, text=True
            )
        fields = dict(
            token.split("=", 1) for token in completed.stdout.strip().split()
        )
        self.assertEqual(fields["deepseek_v4_flash_pto_ops"], "pass")
        self.assertEqual(fields["q8_linear"], "1")
        self.assertEqual(fields["f16_linear"], "1")
        self.assertEqual(fields["rms_norm"], "1")
        self.assertEqual(fields["hc_split"], "1")
        self.assertEqual(fields["hc_weighted_sum"], "1")
        self.assertEqual(fields["hc_post"], "1")
        self.assertEqual(fields["swiglu"], "1")
        self.assertGreater(int(fields["tload"]), 0)
        self.assertGreater(int(fields["tstore"]), 0)


if __name__ == "__main__":
    unittest.main()
