"""Model-free format tests. These fixtures cannot satisfy the real-history gate."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "checkpoint", ROOT / "scripts/prepare_deepseek_history_checkpoint.py")
checkpoint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checkpoint)


def native_fixture():
    positions = 4
    tokens = [42, 201, 41, 201]
    trace = [{"position": p, "input": token, "next": [201, 41, 201, 35][p],
              "elapsed_s": float(p)} for p, token in enumerate(tokens)]
    data = bytearray(struct.pack("<13I", 0x34565344, 2, 2052, 2052,
                                 128, 128, 514, positions, 43, 512, 128, 129280, 4))
    data.extend(struct.pack("<4I", *tokens))
    logits = bytearray(129280 * 4)
    struct.pack_into("<f", logits, 35 * 4, 1.0)
    data.extend(logits)
    attention = [1 if x >= 2 and x % 2 == 0 else 0 for x in range(43)]
    data.extend(struct.pack("<43I", *attention) * 2)
    expected = []
    for layer in range(43):
        ratio = 0 if layer < 2 else (4 if layer % 2 == 0 else 128)
        encoded = bytearray(struct.pack("<Q", ratio))

        def span(elements, marker):
            return struct.pack("<f", float(layer * 10 + marker)) * elements

        def matrix(elements, payload):
            encoded.extend(struct.pack("<Q", elements))
            encoded.extend(payload)

        raw = span(4 * 512, 1)
        data.extend(raw)
        matrix(4 * 512, raw)
        if ratio:
            rows = positions // ratio
            lanes = 2 if ratio == 4 else 1
            elements = ratio * lanes * 512 * lanes
            comp, kv, score = span(rows * 512, 2), span(elements, 3), span(elements, 4)
            data.extend(comp + kv + score)
            matrix(rows * 512, comp)
            if ratio == 4:
                index_elements = ratio * lanes * 128 * lanes
                index, ikv, iscore = (span(rows * 128, 5), span(index_elements, 6),
                                      span(index_elements, 7))
                data.extend(index + ikv + iscore)
                matrix(rows * 128, index)
            encoded.extend(struct.pack("<2Q", 512, ratio))
            matrix(elements, kv)
            matrix(elements, score)
            if ratio == 4:
                encoded.extend(struct.pack("<2Q", 128, ratio))
                matrix(index_elements, ikv)
                matrix(index_elements, iscore)
        expected.append(bytes(encoded))
    return bytes(data), "\n".join(json.dumps(x) for x in trace).encode(), expected


class DeepSeekHistoryCheckpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data, cls.trace, cls.expected = native_fixture()

    def test_all_state_spans_preserved_in_w6_order(self):
        past, token, layers, spans = checkpoint.decode_native(self.data, self.trace)
        self.assertEqual((past, token), (4, 35))
        self.assertEqual(layers, self.expected)
        self.assertEqual(len(spans), 43)
        for first, end in checkpoint.partition(3):
            body = struct.pack("<4Q", checkpoint.STATE_MAGIC, 1, first, end)
            body += b"".join(layers[first:end])
            checkpoint.validate_range(body, past, first, end)

    def test_truncated_and_trailing_native_rejected(self):
        for data in (self.data[:20], self.data[:-1], self.data + b"x"):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                checkpoint.decode_native(data, self.trace)

    def test_wrong_compressed_geometry_rejected(self):
        data = bytearray(self.data)
        struct.pack_into("<I", data, 52 + 16 + 129280 * 4 + 2 * 4, 2)
        with self.assertRaises(ValueError):
            checkpoint.decode_native(data, self.trace)

    def test_trace_chain_and_argmax_rejected(self):
        rows = [json.loads(line) for line in self.trace.splitlines()]
        for index, field, value in ((1, "input", 42), (3, "next", 201),
                                     (2, "position", 1), (2, "elapsed_s", -1)):
            wrong = [dict(row) for row in rows]
            wrong[index][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                checkpoint.decode_native(self.data, b"\n".join(
                    json.dumps(x).encode() for x in wrong))

    def test_all_supported_partitions_cover_exactly_43_layers(self):
        for nodes in (2, 3, 4, 8):
            ranges = checkpoint.partition(nodes)
            self.assertEqual([x for first, end in ranges for x in range(first, end)],
                             list(range(43)))
            self.assertLessEqual(max(end - first for first, end in ranges) -
                                 min(end - first for first, end in ranges), 1)
        with self.assertRaises(ValueError):
            checkpoint.partition(1)

    def test_cli_roundtrip_corruption_and_immutable_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "native").write_bytes(self.data)
            (root / "trace").write_bytes(self.trace)
            cmd = [os.sys.executable, str(ROOT / "scripts/prepare_deepseek_history_checkpoint.py")]
            result = subprocess.run(cmd + ["convert", "--payload", str(root / "native"),
                "--trace", str(root / "trace"), "--output", str(root / "out"), "--nodes", "3"],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            checkpoint.validate(root / "out", 3)
            with self.assertRaises(ValueError):
                checkpoint.validate(root / "out", 2)
            with self.assertRaises(FileExistsError):
                checkpoint.convert(root / "native", root / "trace", root / "out", 3)
            file = root / "out/node0.bin"
            encoded = bytearray(file.read_bytes())
            encoded[-1] ^= 1
            file.write_bytes(encoded)
            result = subprocess.run(cmd + ["validate", "--directory", str(root / "out"),
                                          "--nodes", "3"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checkpoint rejected", result.stderr)

    def test_w6_matrix_metadata_and_eof_checked(self):
        body = struct.pack("<4Q", checkpoint.STATE_MAGIC, 1, 0, 3)
        body += b"".join(self.expected[:3])
        for data in (body[:-1], body + b"x", body[:40] + struct.pack("<Q", 1) + body[48:]):
            with self.assertRaises(ValueError):
                checkpoint.validate_range(data, 4, 0, 3)


class DeepSeekHistoryGuestHeaderTests(unittest.TestCase):
    def test_guest_worker_main_stack_has_headroom(self):
        compiler = shutil.which(os.environ.get("AARCH64_LINUX_CC", "aarch64-linux-gnu-gcc"))
        if not compiler:
            self.skipTest("AArch64 guest compiler is required for stack-usage inspection")
        repo = ROOT.parents[1]
        service = Path(os.environ.get("MEM_SERVICE_ROOT", repo / "mem_service"))
        includes = [ROOT, ROOT.parent, ROOT / "libs/obmm_queue", ROOT / "apps/obmm_queue",
                    ROOT / "libs/lingqu_shmem_pto", ROOT / "libs/obmm_async",
                    repo / "crates/sim-qemu/include", service, service / "libs/obmm_queue",
                    repo / "vendor/obmm/src/libobmm",
                    Path(os.environ.get("KERNEL_UAPI_DIR", ROOT / "out/kernel_uapi/include"))]
        with tempfile.TemporaryDirectory() as directory:
            obj = Path(directory) / "worker.o"
            result = subprocess.run([compiler, "-O2", "-fstack-usage", "-D__EXPORTED_HEADERS__",
                                     "-DLINGQU_SHMEM_MANAGED_MAPPING", "-pthread"] +
                [flag for path in includes for flag in ("-I", str(path))] +
                ["-c", str(ROOT / "apps/llm_infer/llm_infer.c"), "-o", str(obj)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = [line.split("\t") for line in obj.with_suffix(".su").read_text().splitlines()]
            main = [entry for entry in entries if entry[0].endswith(":main")]
            self.assertEqual(len(main), 1)
            self.assertLess(int(main[0][1]), 7 * 1024 * 1024,
                            "Guest main must leave at least 1 MiB of its 8 MiB stack for calls")

    def test_native_guest_envelope_parser(self):
        compiler = shutil.which(os.environ.get("CC", "cc"))
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "probe.c").write_text('''
#include "deepseek_v4_flash_history_guest.h"
int main(int argc, char **argv) {
    struct deepseek_v4_flash_history_header h;
    FILE *f = argc == 2 ? fopen(argv[1], "rb") : NULL;
    int rc = deepseek_v4_flash_history_read(f, &h);
    if (!rc && (h.past != 2051 || h.token != 201 || h.first != 0 ||
        h.end != 3 || h.bytes != 32 || ftell(f) != 64)) rc = -1;
    if (f) fclose(f);
    return rc ? 1 : 0;
}
''')
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "libs/lingqu_shmem_pto"), str(root / "probe.c"),
                            "-o", str(root / "probe")], check=True, capture_output=True)
            header = [checkpoint.MAGIC, 1, 2051, 201, 0, 3, 32, 123]
            good = checkpoint.HEADER.pack(*header) + bytes(32)
            cases = [(good, 0), (good[:-1], 1), (good + b"x", 1)]
            for index, value in ((0, 0), (1, 2), (2, 0), (2, 2**32),
                                  (3, 129280), (4, 3), (5, 44), (6, 8)):
                wrong = list(header)
                wrong[index] = value
                cases.append((checkpoint.HEADER.pack(*wrong) + bytes(32), 1))
            for data, rc in cases:
                (root / "input").write_bytes(data)
                result = subprocess.run([str(root / "probe"), str(root / "input")])
                self.assertEqual(result.returncode, rc)


if __name__ == "__main__":
    unittest.main()
