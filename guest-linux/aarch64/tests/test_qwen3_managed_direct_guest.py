import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class Qwen3ManagedDirectGuestTest(unittest.TestCase):
    def test_qwen_range_geometry_binds_only_managed_tensor_owners(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "qwen3-managed-direct"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(LIB),
                    "-I",
                    str(ROOT / "mem_service"),
                    "-I",
                    str(ROOT / "crates/sim-qemu/include"),
                    str(LIB / "qwen3_managed_direct_guest.c"),
                    str(
                        ROOT
                        / "guest-linux/aarch64/tests"
                        / "qwen3_managed_direct_guest_golden.c"
                    ),
                    "-lm",
                    "-o",
                    str(binary),
                ],
                check=True,
                timeout=30,
            )
            result = subprocess.run(
                [str(binary)],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )

        self.assertEqual(
            result.stdout.strip(),
            "qwen3_managed_direct_guest=pass cases=4 callable=3 staging_bytes=0",
        )

    def test_qwen_direct_contract_has_no_model_payload_pointer(self):
        header = (LIB / "qwen3_managed_direct_guest.h").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("void *payload", header)
        self.assertNotIn("const void *payload", header)
        self.assertIn("*owners[QWEN3_MANAGED_DIRECT_TENSOR_COUNT]", header)
        self.assertIn("QWEN3_MANAGED_DIRECT_CALLABLE_ID", header)


if __name__ == "__main__":
    unittest.main()
