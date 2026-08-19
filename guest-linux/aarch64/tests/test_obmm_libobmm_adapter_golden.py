import pathlib
import shutil
import subprocess
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
AARCH64 = TESTS_DIR.parent
UB_SIM = AARCH64.parent.parent

HARNESS = TESTS_DIR / "obmm_libobmm_adapter_golden.c"
OBMM_SUBMODULE = UB_SIM / "vendor/obmm"
KERNEL_UB = UB_SIM / "guest-linux/kernel_ub"
INCLUDES = [
    "-I", str(AARCH64 / "common"),
    "-I", str(OBMM_SUBMODULE / "src/libobmm"),
    "-I", str(KERNEL_UB / "include/uapi"),
    "-I", str(KERNEL_UB / "include"),
]
SOURCES = [
    str(HARNESS),
    str(OBMM_SUBMODULE / "src/libobmm/libobmm.c"),
    str(AARCH64 / "common/obmm_vendor_adaptor_sim.c"),
]


class ObmmLibobmmAdapterGoldenTest(unittest.TestCase):
    def _compile(self, compiler: str, output: pathlib.Path) -> None:
        subprocess.run(
            [compiler, "-O2", "-Wall", "-Wextra", "-D__EXPORTED_HEADERS__",
             *INCLUDES, *SOURCES,
             "-Wl,--wrap=ioctl", "-Wl,--wrap=open", "-o", str(output)],
            check=True, capture_output=True, text=True,
        )

    def test_adapter_matches_golden_ioctl_bytes(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("cc is unavailable")
        with tempfile.TemporaryDirectory() as temp_dir:
            binary = pathlib.Path(temp_dir) / "adapter_golden"
            self._compile(compiler, binary)
            result = subprocess.run(
                [str(binary)], check=False, capture_output=True, text=True,
            )
            self.assertEqual(0, result.returncode, result.stdout)
            self.assertIn("adapter-golden: ok", result.stdout)
            self.assertNotIn("FAIL", result.stdout)


if __name__ == "__main__":
    unittest.main()
