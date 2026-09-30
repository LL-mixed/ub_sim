import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class ModelDirectGuestTest(unittest.TestCase):
    def test_owned_tensors_remain_pinned_without_payload_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "model-direct"
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
                    str(LIB / "lingqu_model_direct_guest.c"),
                    str(ROOT / "guest-linux/aarch64/tests/model_direct_guest_golden.c"),
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
            "model_direct_guest=pass cases=7 staging_bytes=0",
        )
        self.assertIn("model_tensor_address_space=UB_GM", result.stderr)
        self.assertIn("pto_instruction_io=1", result.stderr)
        self.assertIn("shared_payload_staging_bytes=0", result.stderr)

    def test_public_contract_has_no_model_payload_pointer(self):
        header = (LIB / "lingqu_model_direct_guest.h").read_text(encoding="utf-8")

        self.assertNotIn("void *payload", header)
        self.assertNotIn("const void *payload", header)
        self.assertIn("struct mem_service_mapping_owner *owner", header)
        self.assertIn("struct lingqu_shmem_memref_spec spec", header)


if __name__ == "__main__":
    unittest.main()
