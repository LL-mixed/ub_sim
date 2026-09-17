import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class W5ManagedServingGuestTest(unittest.TestCase):
    def test_serving_app_and_initramfs_link_managed_layers(self):
        makefile = (ROOT / "guest-linux/aarch64/apps/llm_infer/Makefile").read_text()
        initramfs = (ROOT / "guest-linux/aarch64/scripts/build_initramfs.sh").read_text()
        for source in (
                "w5_managed_model_guest.c",
                "w5_managed_object_guest.c",
                "w5_managed_request_guest.c",
                "w5_managed_runtime_obmm.c",
                "w5_managed_serving_guest.c"):
            self.assertIn(source, makefile)
            self.assertIn(source, initramfs)

    def test_callable4_managed_serving_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "managed-serving"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "guest-linux/aarch64"),
                "-I", str(LIB), "-I", str(ROOT / "mem_service"),
                "-I", str(ROOT / "crates/sim-qemu/include"),
                str(LIB / "w5_managed_serving_guest.c"),
                str(ROOT / "guest-linux/aarch64/tests/w5_managed_serving_guest_golden.c"),
                "-o", str(binary),
            ], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    check=True, timeout=10)
            self.assertEqual(
                result.stdout.strip(),
                "w5_managed_serving_guest=pass callable=4 inputs=2 outputs=5 "
                "terminal_v2=1 token_v2=1")
            self.assertEqual(
                result.stderr.strip(),
                "[w5_managed_serving] stage=completion op=41 source=1 "
                "status=3 code=pto_ub_gm_access_denied")


if __name__ == "__main__":
    unittest.main()
