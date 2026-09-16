import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class W5ManagedRuntimeGuestTest(unittest.TestCase):
    def test_provider_adapter_opens_neutral_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "managed-runtime"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "guest-linux/aarch64"),
                "-I", str(LIB), "-I", str(ROOT / "mem_service"),
                "-I", str(ROOT / "crates/sim-qemu/include"),
                str(LIB / "w5_managed_runtime_obmm.c"),
                str(ROOT / "guest-linux/aarch64/tests/w5_managed_runtime_guest_golden.c"),
                "-o", str(binary),
            ], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    check=True, timeout=10)
            self.assertEqual(
                result.stdout.strip(),
                "w5_managed_runtime_guest=pass provider_selected_by_adapter=1 "
                "cleanup=4")


if __name__ == "__main__":
    unittest.main()
