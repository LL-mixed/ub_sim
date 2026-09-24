import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_ROOT = ROOT / "guest-linux" / "aarch64"
LIB_DIR = GUEST_ROOT / "libs" / "lingqu_shmem_pto"
ABI_DIR = ROOT / "crates" / "sim-qemu" / "include"
GOLDEN = GUEST_ROOT / "tests" / "lingqu_shmem_pto_endpoint_golden.c"


def _compile(compiler, output):
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(LIB_DIR),
            "-I",
            str(ABI_DIR),
            str(GOLDEN),
            str(LIB_DIR / "lingqu_shmem_pto_endpoint.c"),
            "-o",
            str(output),
        ],
        check=True,
    )


class LingquShmemPtoEndpointTest(unittest.TestCase):
    def test_multiple_queued_requests_preserve_identity_capacity_and_legacy_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "endpoint-queue"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-I", str(LIB_DIR), "-I", str(ABI_DIR),
                str(GUEST_ROOT / "tests" / "lingqu_shmem_pto_endpoint_queue_golden.c"),
                "-o", str(binary),
            ], check=True)
            for case in ("completion", "capacity", "protocol", "legacy", "replay",
                         "arguments", "threads", "queue-base-mismatch"):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), "--case", case],
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout.strip(),
                                     f"lingqu_shmem_pto_endpoint_queue=pass case={case}")
            self.assertEqual(subprocess.run([str(binary), "--case", "unknown"],
                                           capture_output=True, timeout=5).returncode, 2)

    def test_cancel_submission_uses_op_scoped_mmio_doorbell(self):
        header = (LIB_DIR / "lingqu_shmem_pto_endpoint.h").read_text()
        source = (LIB_DIR / "lingqu_shmem_pto_endpoint.c").read_text()

        self.assertIn("lingqu_shmem_pto_endpoint_submit_cancel_after", header)
        self.assertIn("LINGQU_REG_CANCEL_OP_ID 0x0a0u", source)
        self.assertIn("LINGQU_REG_CANCEL_DOORBELL 0x0a8u", source)
        op_write = source.index("LINGQU_REG_CANCEL_OP_ID, op_id")
        doorbell_write = source.index("LINGQU_REG_CANCEL_DOORBELL, 1")
        self.assertLess(op_write, doorbell_write)
        self.assertIn("cancel_after_ms >= timeout_ms", source)
        self.assertIn("endpoint->cancel_requested = false", source)
        self.assertIn("endpoint->cancel_requested = true", source)

    def test_completion_decoder_golden(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "endpoint-golden"
            _compile("cc", binary)
            output = subprocess.check_output([str(binary)], text=True)
        self.assertEqual(output.strip(), "lingqu_shmem_pto_endpoint_golden=pass")

    def test_timeout_retains_original_operation_and_queues(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "endpoint-recovery"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(LIB_DIR), "-I", str(ABI_DIR),
                str(GUEST_ROOT / "tests" / "lingqu_shmem_pto_endpoint_recovery_golden.c"),
                "-o", str(binary),
            ], check=True)
            output = subprocess.check_output([str(binary)], text=True, timeout=5)
        self.assertEqual(output.strip(), "lingqu_shmem_pto_endpoint_recovery=pass")

    def test_cross_compiles_for_aarch64_without_warnings(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("aarch64 cross compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "endpoint.o"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(LIB_DIR),
                    "-I",
                    str(ABI_DIR),
                    "-c",
                    str(LIB_DIR / "lingqu_shmem_pto_endpoint.c"),
                    "-o",
                    str(output),
                ],
                check=True,
            )
            self.assertEqual(output.read_bytes()[:4], b"\x7fELF")


if __name__ == "__main__":
    unittest.main()
