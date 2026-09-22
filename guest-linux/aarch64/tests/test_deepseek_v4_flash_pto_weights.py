import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "libs" / "lingqu_shmem_pto"


class DeepseekV4FlashPtoWeightsTest(unittest.TestCase):
    def test_read_only_mmap_provider_validates_catalog_and_bounds(self):
        compiler = os.environ.get("CXX", "c++")
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "deepseek_v4_flash_pto_weights_golden"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{LIB}",
                    str(
                        ROOT
                        / "tests"
                        / "deepseek_v4_flash_pto_weights_golden.cpp"
                    ),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            completed = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True
            )
        self.assertEqual(
            completed.stdout.strip(),
            "deepseek_v4_flash_pto_weights=pass cases=4 payload_copy_bytes=0",
        )


if __name__ == "__main__":
    unittest.main()
