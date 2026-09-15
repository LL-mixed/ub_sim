"""Exercise the production result decoder without devices or model weights."""
import ctypes
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
META = 39424
HEADER = META + 64
PAYLOAD = HEADER + 64 + 144
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
                ("node", "first", "end", "next", "nodes", "layers", "hidden")]


class View(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("offset", "bytes", "model_checksum", "service_checksum")]


class Result(ctypes.Structure):
    _fields_ = [("hidden", View), ("kv", View), ("input_checksum", ctypes.c_uint64),
                ("output_tensor_checksum", ctypes.c_uint64),
                ("range_checksum", ctypes.c_uint64), ("real_layers", ctypes.c_uint64)]


def fixture(nodes=2, node=0, layers=28, kv=b"previous-kv"):
    hidden = bytes(range(32))
    record = [node, 1, 2, 1, (node + 1) % nodes, nodes, layers, len(hidden),
              19, tensor_checksum(hidden), 23, 1, 29, 31,
              len(hidden), len(hidden), len(kv), checksum(kv) if kv else 0]
    data = bytearray(PAYLOAD + len(hidden) + len(kv))
    struct.pack_into("<Q", data, META + 40, HEADER)
    struct.pack_into("<8Q", data, HEADER, 0x7133773472667430, 1, 18,
                     144 + len(hidden) + len(kv), 0, 23, 19, tensor_checksum(hidden))
    struct.pack_into("<18Q", data, HEADER + 64, *record)
    data[PAYLOAD:] = hidden + kv
    repair_record_checksum(data)
    return data, Identity(node, 1, 2, (node + 1) % nodes, nodes, layers, len(hidden))


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
                    for view in (result.hidden, result.kv):
                        start = view.offset - 16384
                        payload = data[start:start + view.bytes]
                        self.assertEqual(view.model_checksum, checksum(payload))
                        self.assertEqual(view.service_checksum, checksum(payload, False))
                        self.assertNotEqual(view.model_checksum, view.service_checksum)

    def test_no_kv_returns_no_reference_view(self):
        data, identity = fixture(kv=b"")
        rc, result = self.call(data, identity)
        self.assertEqual(rc, 0)
        self.assertEqual(bytes(result.kv), bytes(ctypes.sizeof(View)))

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
        for field, _ in Identity._fields_:
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
