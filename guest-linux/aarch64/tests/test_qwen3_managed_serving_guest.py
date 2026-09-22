import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class Qwen3ManagedServingGuestTest(unittest.TestCase):
    def test_callable3_uses_managed_readers_and_in_place_writer_views(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "qwen3-managed-serving"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "guest-linux/aarch64"),
                    "-I",
                    str(LIB),
                    "-I",
                    str(ROOT / "mem_service"),
                    "-I",
                    str(ROOT / "crates/sim-qemu/include"),
                    str(LIB / "qwen3_managed_direct_guest.c"),
                    str(LIB / "qwen3_managed_serving_guest.c"),
                    str(
                        ROOT
                        / "guest-linux/aarch64/tests"
                        / "qwen3_managed_serving_guest_golden.c"
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
            "qwen3_managed_serving_guest=pass callable=3 owners=direct "
            "views=nonoverlap publish=in_place outputs=4",
        )

    def test_guest_link_entries_include_w6_qwen_adapter(self):
        makefile = (ROOT / "guest-linux/aarch64/apps/llm_infer/Makefile").read_text()
        initramfs = (ROOT / "guest-linux/aarch64/scripts/build_initramfs.sh").read_text()
        for source in (
            "lingqu_model_direct_guest.c",
            "qwen3_managed_direct_guest.c",
            "qwen3_managed_serving_guest.c",
        ):
            self.assertIn(source, makefile)
            self.assertIn(source, initramfs)


if __name__ == "__main__":
    unittest.main()
