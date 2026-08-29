import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_ROOT = ROOT / "guest-linux" / "aarch64"
LIB_DIR = GUEST_ROOT / "libs" / "lingqu_shmem_pto"
ABI_DIR = ROOT / "crates" / "sim-qemu" / "include"
GOLDEN = GUEST_ROOT / "tests" / "lingqu_shmem_lifetime_golden.c"


def _sources():
    return [
        str(GOLDEN),
        str(LIB_DIR / "lingqu_shmem.c"),
        str(LIB_DIR / "lingqu_shmem_pto_guest.c"),
    ]


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
            *_sources(),
            "-o",
            str(output),
        ],
        check=True,
    )


class LingquShmemLifetimeTest(unittest.TestCase):
    def test_region_memref_and_inflight_lifetime(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "lifetime-golden"
            _compile("cc", binary)
            output = subprocess.check_output([str(binary)], text=True)
        self.assertEqual(output.strip(), "lingqu_shmem_lifetime_golden=pass")

    def test_cross_compiles_for_aarch64_without_warnings(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("aarch64 cross compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "lifetime-golden"
            _compile(compiler, output)
            self.assertEqual(output.read_bytes()[:4], b"\x7fELF")


if __name__ == "__main__":
    unittest.main()
