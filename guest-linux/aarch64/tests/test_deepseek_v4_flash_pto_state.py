import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux" / "aarch64" / "libs" / "lingqu_shmem_pto"


class DeepseekV4FlashPtoStateTest(unittest.TestCase):
    def test_state_layout_is_direct_and_matches_rust_encoding(self):
        compiler = os.environ.get("CXX", "c++")
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "deepseek_v4_flash_pto_state_golden"
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
                    / "deepseek_v4_flash_pto_state_golden.cpp"
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
        self.assertEqual(fields["deepseek_v4_flash_pto_state"], "pass")
        self.assertEqual(fields["previous_bytes"], "618680")
        self.assertEqual(fields["next_bytes"], "625336")
        self.assertEqual(fields["layers"], "2")
        self.assertGreater(int(fields["tload"]), 0)
        self.assertGreater(int(fields["tstore"]), 0)


if __name__ == "__main__":
    unittest.main()
