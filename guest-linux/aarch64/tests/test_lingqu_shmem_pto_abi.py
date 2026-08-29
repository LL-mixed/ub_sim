import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
HEADER = ROOT / "crates" / "sim-qemu" / "include" / "linqu_shmem_pto_abi.h"
QEMU_UBC_HEADER = ROOT / "vendor" / "qemu_8.2.0_ub" / "include" / "hw" / "ub" / "ub_ubc.h"
QEMU_UBC_SOURCE = ROOT / "vendor" / "qemu_8.2.0_ub" / "hw" / "ub" / "ub_ubc.c"


def _compile_header(compiler, language, standard):
    subprocess.run(
        [
            compiler,
            f"-std={standard}",
            "-Werror",
            "-fsyntax-only",
            "-x",
            language,
            "-include",
            str(HEADER),
            "/dev/null",
        ],
        check=True,
    )


class LingquShmemPtoAbiTest(unittest.TestCase):
    def test_compiles_as_c_and_cpp(self):
        _compile_header("cc", "c", "c11")
        _compile_header("c++", "c++", "c++17")

    def test_uses_the_default_ub_gm_contract(self):
        source = HEADER.read_text()
        self.assertIn("LingquPtoDispatchControlV2", source)
        self.assertIn("LingquShmemMemrefV1", source)
        self.assertIn("PtoSimUbGmAccessOpsV1", source)
        self.assertNotIn("EXTERNAL_GM", source)
        self.assertNotIn("external_memref", source)
        self.assertNotIn("NPU_OP_", source)

    def test_qemu_ubc_reserves_an_explicit_default_disabled_pto_cna(self):
        header = QEMU_UBC_HEADER.read_text()
        source = QEMU_UBC_SOURCE.read_text()
        self.assertIn("uint32_t pto_device_cna;", header)
        self.assertIn(
            'DEFINE_PROP_UINT32("pto-device-cna", BusControllerDev, pto_device_cna, 0)',
            source,
        )


if __name__ == "__main__":
    unittest.main()
