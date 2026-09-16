"""Exercise the production result decoder without devices or model weights."""
import ctypes
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[3]
META = 39424
HEADER = META + 64
PAYLOAD = HEADER + 64 + 144
LOGITS = 1024
LOGITS_ENTRY = LOGITS + 64
TOKEN_TEXT = LOGITS_ENTRY + 360
TOKEN_TEXT_ENTRY = TOKEN_TEXT + 64
MASK = (1 << 64) - 1


def checksum(data, positional=True):
    value = 0xCBF29CE484222325 if positional else 1469598103934665603
    for index, byte in enumerate(data):
        value = ((value ^ (byte | (index << 8 if positional else 0))) *
                 1099511628211) & MASK
    return value


def tensor_checksum(data):
    value = 0xCBF29CE484222325
    for start in range(0, len(data), 8):
        value = ((value ^ int.from_bytes(data[start:start + 8], "little")) *
                 1099511628211) & MASK
    return value


class Identity(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("node", "first", "end", "next", "nodes", "layers", "hidden",
                 "step", "vocab")]


class View(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("offset", "bytes", "model_checksum", "service_checksum")]


class Terminal(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("step", "sampled_token", "runner_up_token", "margin_milli",
                 "logits_checksum", "text_checksum", "top_logit_bits",
                 "runner_up_logit_bits", "full_vocab_checked_token_count",
                 "full_vocab_logits_checksum", "piece_bytes", "piece_word0",
                 "piece_word1", "candidate_count")] + [
        (name, ctypes.c_uint64 * 4) for name in
        ("candidate_tokens", "candidate_logit_bits", "candidate_text_checksums",
         "candidate_piece_bytes", "candidate_piece_word0", "candidate_piece_word1")]


class Result(ctypes.Structure):
    _fields_ = [("hidden", View), ("kv", View), ("terminal_view", View),
                ("input_checksum", ctypes.c_uint64),
                ("output_tensor_checksum", ctypes.c_uint64),
                ("range_checksum", ctypes.c_uint64), ("real_layers", ctypes.c_uint64),
                ("serialized_bytes", ctypes.c_uint64),
                ("serialized_checksum", ctypes.c_uint64),
                ("terminal_present", ctypes.c_uint64), ("terminal", Terminal)]


def add_terminal_metadata(data, step, vocab):
    sampled = 11
    runner_up = 12
    text_checksum = 0x4567
    piece = int.from_bytes(b"tok", "little")
    runner_piece = int.from_bytes(b"run", "little")
    struct.pack_into("<4Q", data, LOGITS, 0x713377346c6f6730, 1, 45, 360)
    values = {
        16: 7, 24: vocab, 32: sampled, 40: runner_up, 48: 25,
        56: 0x1234, 64: text_checksum, 72: step, 104: vocab,
        112: 0x2345, 120: 0x3f800000, 128: 0x3f000000,
        136: 1, 144: 0x3456, 152: 0x4567, 160: 2,
    }
    for offset, value in values.items():
        struct.pack_into("<Q", data, LOGITS_ENTRY + offset, value)
    candidates = [
        (sampled, 0x3f800000, text_checksum, 3, piece, 0),
        (runner_up, 0x3f000000, 0x5678, 3, runner_piece, 0),
    ]
    for index, candidate in enumerate(candidates):
        struct.pack_into("<6Q", data, LOGITS_ENTRY + 168 + index * 48, *candidate)
    struct.pack_into("<7Q", data, TOKEN_TEXT, 0x7133773474787430, 1, 8, 64,
                     3, 0x6789, 2)
    struct.pack_into("<8Q", data, TOKEN_TEXT_ENTRY, step, sampled, 0, 3,
                     piece, 0, text_checksum, 3)


def fixture(nodes=2, node=0, layers=28, kv=b"previous-kv", terminal=False,
            step=0, vocab=151936):
    hidden = bytes(range(32))
    first = layers - 1 if terminal else 1
    end = layers if terminal else 2
    record = [node, first, end, end - first, (node + 1) % nodes, nodes, layers, len(hidden),
              19, tensor_checksum(hidden), 23, 1, 29, 31,
              len(hidden), len(hidden), len(kv), checksum(kv) if kv else 0]
    data = bytearray(PAYLOAD + len(hidden) + len(kv))
    struct.pack_into("<Q", data, META + 40, HEADER)
    struct.pack_into("<8Q", data, HEADER, 0x7133773472667430, 1, 18,
                     144 + len(hidden) + len(kv), 0, 23, 19, tensor_checksum(hidden))
    struct.pack_into("<18Q", data, HEADER + 64, *record)
    data[PAYLOAD:] = hidden + kv
    if terminal:
        add_terminal_metadata(data, step, vocab)
    repair_record_checksum(data)
    return data, Identity(node, first, end, (node + 1) % nodes, nodes, layers,
                          len(hidden), step, vocab)


def repair_record_checksum(data):
    struct.pack_into("<Q", data, HEADER + 32,
                     checksum(data[HEADER + 64:PAYLOAD]))


class ManagedResultTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        library = pathlib.Path(cls.directory.name) / "result.so"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC",
                        str(ROOT / "guest-linux/aarch64/components/llm_infer/llm_infer.c"),
                        "-o", str(library)], check=True, timeout=30)
        cls.library = ctypes.CDLL(str(library))
        cls.decode = cls.library.llm_infer_decode_range_result
        cls.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64,
                               ctypes.POINTER(Identity), ctypes.POINTER(Result)]
        cls.decode.restype = ctypes.c_int
        cls.object_key = cls.library.llm_infer_managed_object_key
        cls.object_key.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_uint64, ctypes.c_uint64,
                                   ctypes.c_uint32, ctypes.c_uint64]
        cls.object_key.restype = ctypes.c_int
        cls.pipeline_nodes = cls.library.llm_infer_qwen3_pipeline_nodes
        cls.pipeline_nodes.restype = ctypes.c_uint64
        cls.pipeline_enabled = cls.library.llm_infer_qwen3_range_pipeline_enabled
        cls.pipeline_enabled.argtypes = [ctypes.c_uint32]
        cls.pipeline_enabled.restype = ctypes.c_bool

    def test_pipeline_width_is_independent_of_tensor_shard_count(self):
        for nodes in (2, 3, 8):
            with self.subTest(nodes=nodes), mock.patch.dict(os.environ, {
                "SIM_W5_CLUSTER_NODE_COUNT": str(nodes),
                "SIM_QWEN3_DENSE_TP_NODES": "8",
            }):
                self.assertEqual(self.pipeline_nodes(), nodes)
        with mock.patch.dict(os.environ, {
            "SIM_W5_CLUSTER_NODE_COUNT": "",
            "SIM_QWEN3_DENSE_TP_NODES": "8",
        }):
            self.assertEqual(self.pipeline_nodes(), 8)

    def test_guest_range_gate_uses_pipeline_width_not_tensor_shards(self):
        for nodes in (2, 3, 4, 8):
            with self.subTest(nodes=nodes), mock.patch.dict(os.environ, {
                "SIM_W5_CLUSTER_NODE_COUNT": str(nodes),
                "SIM_QWEN3_DENSE_TP_NODES": "8",
                "SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS": "28",
            }):
                self.assertTrue(self.pipeline_enabled(nodes))
                self.assertFalse(self.pipeline_enabled(8 if nodes != 8 else 3))
        with mock.patch.dict(os.environ, {
            "SIM_W5_CLUSTER_NODE_COUNT": "3",
            "SIM_QWEN3_DENSE_TP_NODES": "8",
            "SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS": "2",
        }):
            self.assertFalse(self.pipeline_enabled(3))
        with mock.patch.dict(os.environ, {
            "SIM_W5_CLUSTER_NODE_COUNT": "8",
            "SIM_QWEN3_DENSE_TP_NODES": "8",
            "SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS": "28",
        }):
            for unsupported in (0, 1, 5, 9):
                self.assertFalse(self.pipeline_enabled(unsupported))
        with mock.patch.dict(os.environ, {
            "SIM_W5_CLUSTER_NODE_COUNT": "5",
            "SIM_QWEN3_DENSE_TP_NODES": "8",
            "SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS": "28",
        }):
            self.assertFalse(self.pipeline_enabled(5))

    def test_canonical_managed_serving_key_and_untouched_failures(self):
        key = ctypes.create_string_buffer(65)
        self.assertEqual(self.object_key(key, 65, 1, 2, 3, 4), 0)
        self.assertEqual(key.value,
                         b"w5v1/0000000000000001/0000000000000002/00000003/"
                         b"0000000000000004")
        for run, request, size in ((0, 2, 65), (1, 0, 65), (1, 2, 64)):
            with self.subTest(run=run, request=request, size=size):
                output = ctypes.create_string_buffer(b"x" * 65, 65)
                self.assertNotEqual(self.object_key(output, size, run, request, 3, 4), 0)
                self.assertEqual(output.raw, b"x" * 65)
        self.assertEqual(self.object_key(key, 65, (1 << 64) - 1,
                                         (1 << 64) - 1, (1 << 32) - 1,
                                         (1 << 64) - 1), 0)
        self.assertEqual(len(key.value), 64)

    def call(self, data, identity, offset=16384, length=None):
        buffer = ctypes.create_string_buffer(bytes(data))
        result = Result()
        ctypes.memset(ctypes.byref(result), 0xA5, ctypes.sizeof(result))
        rc = self.decode(buffer, len(data) if length is None else length,
                         offset, ctypes.byref(identity), ctypes.byref(result))
        self.assertEqual(buffer.raw[:len(data)], bytes(data), "decoder changed payload")
        if rc:
            self.assertEqual(bytes(result), bytes(ctypes.sizeof(result)),
                             "rejected result exposed a partial subview")
        return rc, result

    def test_returns_allocation_relative_views_and_distinct_checksums(self):
        for nodes in (2, 3, 8):
            for layers in (28, 43):
                with self.subTest(nodes=nodes, layers=layers):
                    data, identity = fixture(nodes, nodes - 1, layers)
                    rc, result = self.call(data, identity)
                    self.assertEqual(rc, 0)
                    self.assertEqual(result.hidden.offset, 16384 + PAYLOAD)
                    self.assertEqual(result.hidden.bytes, 32)
                    self.assertEqual(result.kv.offset, result.hidden.offset + 32)
                    self.assertEqual(result.kv.bytes, len(b"previous-kv"))
                    self.assertEqual(result.serialized_bytes, len(data))
                    self.assertEqual(result.serialized_checksum,
                                     checksum(data, False))
                    self.assertEqual(bytes(result.terminal_view),
                                     bytes(ctypes.sizeof(View)))
                    self.assertEqual(result.terminal_present, 0)
                    for view in (result.hidden, result.kv):
                        start = view.offset - 16384
                        payload = data[start:start + view.bytes]
                        self.assertEqual(view.model_checksum, checksum(payload))
                        self.assertEqual(view.service_checksum, checksum(payload, False))
                        self.assertNotEqual(view.model_checksum, view.service_checksum)

    def test_reports_exact_serialized_bytes_when_capacity_is_larger(self):
        data, identity = fixture()
        serialized_bytes = len(data)
        data.extend(b"\xa5" * 4096)
        rc, result = self.call(data, identity)
        self.assertEqual(rc, 0)
        self.assertEqual(result.serialized_bytes, serialized_bytes)
        self.assertEqual(result.serialized_checksum,
                         checksum(data[:serialized_bytes], False))
        self.assertLess(result.serialized_bytes, len(data))
        self.assertEqual(result.kv.offset + result.kv.bytes,
                         16384 + result.serialized_bytes)

    def test_no_kv_returns_no_reference_view(self):
        data, identity = fixture(kv=b"")
        rc, result = self.call(data, identity)
        self.assertEqual(rc, 0)
        self.assertEqual(bytes(result.kv), bytes(ctypes.sizeof(View)))

    def test_terminal_range_returns_step_vocab_and_candidate_metadata(self):
        data, identity = fixture(nodes=3, node=2, layers=43, terminal=True, step=7,
                                 vocab=129280)
        rc, result = self.call(data, identity)
        self.assertEqual(rc, 0)
        self.assertEqual(result.terminal_present, 1)
        terminal_bytes = TOKEN_TEXT_ENTRY + 64 - LOGITS
        self.assertEqual(result.terminal_view.offset, 16384 + LOGITS)
        self.assertEqual(result.terminal_view.bytes, terminal_bytes)
        self.assertEqual(result.terminal_view.model_checksum, 0x1234)
        self.assertEqual(
            result.terminal_view.service_checksum,
            checksum(data[LOGITS:LOGITS + terminal_bytes], False))
        terminal = result.terminal
        self.assertEqual(terminal.step, 7)
        self.assertEqual(terminal.sampled_token, 11)
        self.assertEqual(terminal.runner_up_token, 12)
        self.assertEqual(terminal.margin_milli, 25)
        self.assertEqual(terminal.logits_checksum, 0x1234)
        self.assertEqual(terminal.text_checksum, 0x4567)
        self.assertEqual(terminal.full_vocab_checked_token_count, 129280)
        self.assertEqual(terminal.full_vocab_logits_checksum, 0x2345)
        self.assertEqual(terminal.piece_bytes, 3)
        self.assertEqual(terminal.piece_word0, int.from_bytes(b"tok", "little"))
        self.assertEqual(terminal.candidate_count, 2)
        self.assertEqual(list(terminal.candidate_tokens)[:2], [11, 12])
        self.assertEqual(list(terminal.candidate_text_checksums)[:2],
                         [0x4567, 0x5678])

    def test_terminal_range_requires_unique_table_for_expected_step_and_vocab(self):
        data, identity = fixture(terminal=True, step=4)
        for field in ("step", "vocab"):
            changed = Identity.from_buffer_copy(identity)
            setattr(changed, field, getattr(changed, field) + 1)
            with self.subTest(field=field):
                self.assertNotEqual(self.call(data, changed)[0], 0)
        missing = bytearray(data)
        struct.pack_into("<Q", missing, LOGITS, 0)
        self.assertNotEqual(self.call(missing, identity)[0], 0)
        duplicate = bytearray(data)
        block = duplicate[LOGITS:TOKEN_TEXT_ENTRY + 64]
        duplicate[2048:2048 + len(block)] = block
        self.assertNotEqual(self.call(duplicate, identity)[0], 0)

    def test_terminal_range_rejects_inconsistent_logits_and_token_text(self):
        data, identity = fixture(terminal=True, step=4)
        mutations = (
            (LOGITS + 16, 44),
            (LOGITS_ENTRY + 24, identity.vocab + 1),
            (LOGITS_ENTRY + 32, identity.vocab),
            (LOGITS_ENTRY + 72, identity.step + 1),
            (LOGITS_ENTRY + 104, identity.vocab - 1),
            (LOGITS_ENTRY + 160, 0),
            (LOGITS_ENTRY + 168 + 16, 0x9999),
            (TOKEN_TEXT + 32, 4),
            (TOKEN_TEXT_ENTRY, identity.step + 1),
            (TOKEN_TEXT_ENTRY + 8, 13),
            (TOKEN_TEXT_ENTRY + 24, 17),
            (TOKEN_TEXT_ENTRY + 48, 0x9999),
        )
        for offset, value in mutations:
            changed = bytearray(data)
            struct.pack_into("<Q", changed, offset, value)
            with self.subTest(offset=offset):
                self.assertNotEqual(self.call(changed, identity)[0], 0)

    def test_rejects_truncation_overflow_and_bad_pointer_without_scanning(self):
        data, identity = fixture()
        for length in (0, META + 47, HEADER + 63, PAYLOAD - 1, len(data) - 1):
            with self.subTest(length=length):
                self.assertNotEqual(self.call(data[:length], identity)[0], 0)
        self.assertNotEqual(self.call(data, identity, MASK - len(data) + 1)[0], 0)
        for pointer in (0, 8, HEADER + 1, len(data) - 63, MASK):
            changed = bytearray(data)
            struct.pack_into("<Q", changed, META + 40, pointer)
            with self.subTest(pointer=pointer):
                self.assertNotEqual(self.call(changed, identity)[0], 0)

    def test_rejects_every_header_field_and_entry_checksum(self):
        data, identity = fixture()
        for index in range(8):
            changed = bytearray(data)
            changed[HEADER + index * 8] ^= 1
            with self.subTest(header_word=index):
                self.assertNotEqual(self.call(changed, identity)[0], 0)
        changed = bytearray(data)
        changed[HEADER + 64 + 12 * 8] ^= 1
        self.assertNotEqual(self.call(changed, identity)[0], 0)

    def test_rejects_semantically_wrong_entries_even_with_valid_table_checksum(self):
        data, identity = fixture()
        for index in range(18):
            changed = bytearray(data)
            old = struct.unpack_from("<Q", data, HEADER + 64 + index * 8)[0]
            # Layer numerical checksums differ from serialized tensor metadata.
            value = 0 if index in (12, 13) else old ^ 1
            struct.pack_into("<Q", changed, HEADER + 64 + index * 8, value)
            repair_record_checksum(changed)
            with self.subTest(entry_word=index):
                self.assertNotEqual(self.call(changed, identity)[0], 0)

    def test_separates_hidden_tensor_metadata_and_rejects_corrupt_kv(self):
        data, identity = fixture()
        changed = bytearray(data)
        changed[-1] ^= 1
        self.assertNotEqual(self.call(changed, identity)[0], 0)
        changed = bytearray(data)
        struct.pack_into("<Q", changed, HEADER + 64 + 17 * 8,
                         checksum(data[PAYLOAD + 32:], False))
        repair_record_checksum(changed)
        self.assertNotEqual(self.call(changed, identity)[0], 0)
        rc, result = self.call(data, identity)
        self.assertEqual(rc, 0)
        self.assertEqual(result.output_tensor_checksum, tensor_checksum(data[PAYLOAD:PAYLOAD + 32]))
        self.assertNotEqual(result.output_tensor_checksum, result.hidden.model_checksum)

    def test_rejects_wrong_expected_dispatch_and_null_arguments(self):
        data, identity = fixture()
        for field, _ in Identity._fields_[:7]:
            changed = Identity.from_buffer_copy(identity)
            setattr(changed, field, getattr(changed, field) + 1)
            with self.subTest(field=field):
                self.assertNotEqual(self.call(data, changed)[0], 0)
        buffer = ctypes.create_string_buffer(bytes(data))
        result = Result()
        self.assertNotEqual(self.decode(None, len(data), 0, ctypes.byref(identity),
                                       ctypes.byref(result)), 0)
        self.assertNotEqual(self.decode(buffer, len(data), 0, None, ctypes.byref(result)), 0)
        self.assertNotEqual(self.decode(buffer, len(data), 0, ctypes.byref(identity), None), 0)


if __name__ == "__main__":
    unittest.main()
