import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LIB = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"


class DeepseekV4FlashManagedServingGuestTest(unittest.TestCase):
    def test_callable5_uses_managed_readers_and_in_place_writer_views(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "deepseek-managed-serving"
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
                    str(LIB / "deepseek_v4_flash_managed_direct_guest.c"),
                    str(LIB / "deepseek_v4_flash_managed_serving_guest.c"),
                    str(
                        ROOT
                        / "guest-linux/aarch64/tests"
                        / "deepseek_v4_flash_managed_serving_guest_golden.c"
                    ),
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
            "deepseek_v4_flash_managed_serving_guest=pass callable=5 "
            "owners=direct views=nonoverlap publish=in_place outputs=4",
        )

    def test_guest_link_entries_include_deepseek_direct_serving(self):
        makefile = (
            ROOT / "guest-linux/aarch64/apps/llm_infer/Makefile"
        ).read_text()
        initramfs = (
            ROOT / "guest-linux/aarch64/scripts/build_initramfs.sh"
        ).read_text()
        for source in (
            "deepseek_v4_flash_managed_direct_guest.c",
            "deepseek_v4_flash_managed_serving_guest.c",
        ):
            self.assertIn(source, makefile)
            self.assertIn(source, initramfs)


if __name__ == "__main__":
    unittest.main()
