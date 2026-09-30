"""Run the production CMDQ diagnostic block with its real descriptor layout."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class CmdqDiagnosticTests(unittest.TestCase):
    def test_single_descriptor_logging_stays_inside_payload(self):
        compiler = (os.environ.get("CC") or
                    (shutil.which("gcc-15") if sys.platform.startswith("linux") else None) or
                    shutil.which("cc"))
        if not compiler:
            self.skipTest("requires C compiler with undefined behavior sanitizer")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c").read_text()
        descriptor = "typedef struct QEMU_PACKED UBCCmdqDesc {" + source.split(
            "typedef struct QEMU_PACKED UBCCmdqDesc {", 1
        )[1].split("} UBCCmdqDesc;", 1)[0] + "} UBCCmdqDesc;"
        diagnostic = source.split(
            "/* Diagnostic: verify ue2ue ctrlq response reached guest RAM */", 1
        )[1].split("/* Push ue2ue ctrlq response to CMDQ CRQ", 1)[0]
        program = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define QEMU_PACKED __attribute__((packed))
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define MEMTX_OK 0
#define UBASE_OPC_UE2UE_UBASE 1
''' + descriptor + r'''
static UBCCmdqDesc descs[1] = {{.data={1,2,3,4,5,6}}};
static struct { unsigned depth; } queue = {64}, *csq=&queue;
static void *ubc_dev;
static int read_result;
static int ubc_read_desc(void *q, void *dev, unsigned index, UBCCmdqDesc *out)
{ (void)q; (void)dev; (void)index; *out=descs[0]; return read_result; }
int main(int argc, char **argv) {
    unsigned opcode=1, bd_num=1, idx=1;
    read_result = argc > 1 ? atoi(argv[1]) : 0;
''' + diagnostic + r'''
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(program)
            binary = path / "test"
            build = subprocess.run(
                [compiler, "-std=c11", "-O1", "-Wall", "-Wextra",
                 "-fsanitize=undefined", "-fno-sanitize-recover=all",
                 str(path / "test.c"), "-o", str(binary)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for status in (0, 1):
                with self.subTest(read_result=status):
                    result = subprocess.run([str(binary), str(status)],
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn("runtime error", result.stderr)
                    self.assertIn("data[4..5]=[00000005 00000006]", result.stderr)
                    self.assertIn("readback[" if not status else "readback FAILED", result.stderr)
