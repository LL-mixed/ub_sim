import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class W5ManagedRequestGuestTest(unittest.TestCase):
    def test_final_token_barrier_and_request_retirement(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "managed-request"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(LIB), "-I", str(ROOT / "mem_service"),
                str(LIB / "w5_managed_request_guest.c"),
                str(ROOT / "guest-linux/aarch64/tests/"
                    "w5_managed_request_guest_golden.c"),
                "-o", str(binary),
            ], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    check=True, timeout=10)
            self.assertEqual(
                result.stdout.strip(),
                "w5_managed_request_guest=pass terminal_barrier=1 "
                "local_retire=2 request_retired=6 resource_baseline=1")


if __name__ == "__main__":
    unittest.main()
