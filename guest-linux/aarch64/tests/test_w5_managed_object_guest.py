import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class W5ManagedObjectGuestTest(unittest.TestCase):
    def test_writer_reader_owner_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "managed-object"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(LIB), "-I", str(ROOT / "mem_service"),
                str(LIB / "w5_managed_object_guest.c"),
                str(ROOT / "guest-linux/aarch64/tests/w5_managed_object_guest_golden.c"),
                "-o", str(binary),
            ], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    check=True, timeout=10)
            self.assertEqual(
                result.stdout.strip(),
                "w5_managed_object_guest=pass writer_lifecycle=1 "
                "reader_lifecycle=1 reader_timeout_retry=2 writer_abort=1 "
                "owner_cleanup=1")
            self.assertIn(
                "[w5_managed_object] role=writer stage=map rc=-5 status=10 ",
                result.stderr)


if __name__ == "__main__":
    unittest.main()
