import binascii
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_ROOT = ROOT / "guest-linux" / "aarch64"
LIB_DIR = GUEST_ROOT / "libs" / "lingqu_shmem_pto"
ABI_DIR = ROOT / "crates" / "sim-qemu" / "include"
GOLDEN = GUEST_ROOT / "tests" / "lingqu_shmem_pto_guest_golden.c"


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
            str(LIB_DIR / "lingqu_shmem_pto_guest.c"),
            "-o",
            str(output),
        ],
        check=True,
    )


class LingquShmemPtoGuestTest(unittest.TestCase):
    def test_native_golden_matches_independent_wire_oracle(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "golden"
            _compile("cc", binary)
            lines = subprocess.check_output([str(binary)], text=True).splitlines()

        summary = dict(field.split("=", 1) for field in lines[0].split())
        metadata = bytes.fromhex(lines[1])
        slot = bytes.fromhex(lines[2])
        self.assertEqual(int(summary["bytes"]), 328)
        self.assertEqual(len(metadata), 328)
        self.assertEqual(len(slot), 64)

        mapping_ref = 0x703
        control = bytearray(64)
        struct.pack_into(
            "<IIQQIIQQQII",
            control,
            0,
            2,
            64,
            0x8877665544332211,
            1,
            3,
            0,
            0x100040,
            0,
            0xA1A2A3A4A5A6A7A8,
            0,
            0x1234,
        )
        memref_table = bytearray()
        views = bytearray()
        for index in range(3):
            shape_iova = 0x100130 + index * 8
            stride_iova = shape_iova + 4
            memref_table.extend(
                struct.pack(
                    "<IIQQQQQQIIHBBIII",
                    1,
                    80,
                    mapping_ref,
                    0x200000,
                    index * 64,
                    64,
                    shape_iova,
                    stride_iova,
                    index,
                    1,
                    0,
                    1 if index < 2 else 2,
                    1 if index < 2 else 2,
                    0,
                    0,
                    0,
                )
            )
            views.extend(struct.pack("<II", 16, 1))
        crc = binascii.crc32(control + memref_table + views) & 0xFFFFFFFF
        struct.pack_into("<I", control, 56, crc)
        expected_metadata = bytes(control + memref_table + views)
        expected_slot = bytearray(64)
        expected_slot[0] = 10
        struct.pack_into("<Q", expected_slot, 1, 0x1122334455667788)
        struct.pack_into("<Q", expected_slot, 9, 0x100000)

        self.assertEqual(int(summary["crc"], 16), crc)
        self.assertEqual(int(summary["mapping_ref"], 16), mapping_ref)
        self.assertEqual(metadata, expected_metadata)
        self.assertEqual(slot, bytes(expected_slot))

    def test_cross_compiles_for_aarch64_without_warnings(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("aarch64 cross compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "golden.o"
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
                    str(LIB_DIR / "lingqu_shmem_pto_guest.c"),
                    "-o",
                    str(output),
                ],
                check=True,
            )
            self.assertEqual(output.read_bytes()[:4], b"\x7fELF")


if __name__ == "__main__":
    unittest.main()
