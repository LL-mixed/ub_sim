import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class W5ManagedModelGuestTest(unittest.TestCase):
    def test_operation_retains_owners_and_control_codec(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "managed-model"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(LIB), "-I", str(ROOT / "mem_service"),
                "-I", str(ROOT / "crates/sim-qemu/include"),
                str(LIB / "w5_managed_model_guest.c"),
                str(ROOT / "guest-linux/aarch64/tests/w5_managed_model_guest_golden.c"),
                "-o", str(binary),
            ], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    check=True, timeout=10)
            self.assertEqual(result.stdout.strip(),
                "w5_managed_model_guest=pass cases=24 retained_until_original_completion=1")
            control = subprocess.check_output([str(binary), "--control"], timeout=10)
            expected = bytearray(64 + 8192 + 512)
            struct.pack_into("<QIIQQ8I", expected, 0, 0x314D474D3557514C, 1, 0,
                             17, 19, 0x5133060B, 0, 0, 1, 1, 2, 28, 2048)
            expected[64:72] = b"Q3PROMPT"
            self.assertEqual(control, expected)
