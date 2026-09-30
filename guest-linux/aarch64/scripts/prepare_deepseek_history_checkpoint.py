#!/usr/bin/env python3
"""Test-only, lossless DS4 CPU session -> W6 managed range checkpoint adapter."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

MAGIC = 0x4453344849535446
STATE_MAGIC = 0x4453345253544154
HEADER = struct.Struct("<8Q")
VOCAB = 129280
LAYERS = 43


def checksum(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def digest(data):
    return hashlib.sha256(data).hexdigest()


def partition(nodes):
    if nodes not in (2, 3, 4, 8):
        raise ValueError("nodes must be 2, 3, 4 or 8")
    base, rem = divmod(LAYERS, nodes)
    return [(n * base + min(n, rem), (n + 1) * base + min(n + 1, rem))
            for n in range(nodes)]


class Cursor:
    def __init__(self, data):
        self.data = memoryview(data)
        self.offset = 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.offset:
            raise ValueError(f"truncated checkpoint at {self.offset}")
        result = self.data[self.offset:self.offset + size]
        self.offset += size
        return result

    def integers(self, count, code="I"):
        fmt = struct.Struct("<" + code * count)
        return fmt.unpack(self.take(fmt.size))

    def finish(self):
        if self.offset != len(self.data):
            raise ValueError("trailing checkpoint bytes")


def decode_native(data, trace_data):
    cursor = Cursor(data)
    h = cursor.integers(13)
    if (h[:2] != (0x34565344, 2) or h[8:12] != (43, 512, 128, VOCAB)
            or h[7] < 1 or h[7] >= 2**32 - 1 or h[2] < h[7]
            or h[5] != 128 or h[12] != min(h[7], 128)
            or h[4] < h[12]):
        raise ValueError("unsupported native session geometry")
    positions = h[7]
    tokens = cursor.integers(positions)
    logits = array("f")
    logits.frombytes(cursor.take(VOCAB * 4))
    if sys.byteorder != "little":
        logits.byteswap()
    if any(not math.isfinite(x) for x in logits):
        raise ValueError("nonfinite final logits")
    trace = [json.loads(line) for line in trace_data.splitlines()]
    if len(trace) != positions:
        raise ValueError("trace length differs from native history")
    for pos, row in enumerate(trace):
        if (row["position"] != pos or row["input"] != tokens[pos]
                or not 0 <= tokens[pos] < VOCAB or not 0 <= row["next"] < VOCAB
                or not math.isfinite(row["elapsed_s"]) or row["elapsed_s"] < 0
                or (pos and (trace[pos - 1]["next"] != tokens[pos]
                             or trace[pos - 1]["elapsed_s"] > row["elapsed_s"]))):
            raise ValueError(f"invalid autoregressive trace at {pos}")
    if trace[-1]["next"] != max(range(VOCAB), key=logits.__getitem__):
        raise ValueError("next token differs from final logits argmax")
    attention = cursor.integers(LAYERS)
    indexer = cursor.integers(LAYERS)
    layers = []
    spans = []
    for layer in range(LAYERS):
        start = cursor.offset
        ratio = 0 if layer < 2 else (4 if layer % 2 == 0 else 128)
        rows = positions // ratio if ratio else 0
        if (attention[layer] != rows or indexer[layer] != (rows if ratio == 4 else 0)
                or rows > h[6]):
            raise ValueError(f"invalid compressed row count at layer {layer}")
        encoded = bytearray(struct.pack("<Q", ratio))

        def matrix(elements, payload):
            encoded.extend(struct.pack("<Q", elements))
            encoded.extend(payload)

        matrix(h[12] * 512, cursor.take(h[12] * 512 * 4))
        if ratio:
            comp = cursor.take(rows * 512 * 4)
            lanes = 2 if ratio == 4 else 1
            attn_elements = ratio * lanes * 512 * lanes
            attn_kv = cursor.take(attn_elements * 4)
            attn_score = cursor.take(attn_elements * 4)
            matrix(rows * 512, comp)
            if ratio == 4:
                matrix(rows * 128, cursor.take(rows * 128 * 4))
                index_elements = ratio * lanes * 128 * lanes
                index_kv = cursor.take(index_elements * 4)
                index_score = cursor.take(index_elements * 4)
            encoded.extend(struct.pack("<2Q", 512, ratio))
            matrix(attn_elements, attn_kv)
            matrix(attn_elements, attn_score)
            if ratio == 4:
                encoded.extend(struct.pack("<2Q", 128, ratio))
                matrix(index_elements, index_kv)
                matrix(index_elements, index_score)
        layers.append(encoded)
        spans.append({"layer": layer, "ratio": ratio, "raw_rows": h[12],
                      "compressed_rows": rows, "indexer_rows": indexer[layer],
                      "native_bytes": cursor.offset - start,
                      "native_sha256": digest(cursor.data[start:cursor.offset]),
                      "range_bytes": len(encoded), "range_sha256": digest(encoded)})
    cursor.finish()
    return positions, trace[-1]["next"], layers, spans


def validate_range(data, past, first, end):
    cursor = Cursor(data)
    if cursor.integers(4, "Q") != (STATE_MAGIC, 1, first, end):
        raise ValueError("invalid W6 state header")
    for layer in range(first, end):
        ratio = 0 if layer < 2 else (4 if layer % 2 == 0 else 128)
        if cursor.integers(1, "Q")[0] != ratio:
            raise ValueError("invalid W6 compression ratio")

        def matrix(elements):
            if cursor.integers(1, "Q")[0] != elements:
                raise ValueError("invalid W6 matrix element count")
            cursor.take(elements * 4)

        matrix(min(past, 128) * 512)
        if ratio:
            matrix((past // ratio) * 512)
            if ratio == 4:
                matrix((past // ratio) * 128)
            for head in ([512, 128] if ratio == 4 else [512]):
                if cursor.integers(2, "Q") != (head, ratio):
                    raise ValueError("invalid W6 compressor geometry")
                lanes = 2 if ratio == 4 else 1
                matrix(ratio * lanes * head * lanes)
                matrix(ratio * lanes * head * lanes)
    cursor.finish()


def convert(payload, trace, output, nodes):
    native = payload.read_bytes()
    trace_data = trace.read_bytes()
    past, token, layers, spans = decode_native(native, trace_data)
    cuts = partition(nodes)
    receipt = {"format": "w6-deepseek-real-history-v1", "nodes": nodes,
               "positions": past, "next_token": token,
               "native_sha256": digest(native), "trace_sha256": digest(trace_data),
               "source_payload": str(payload), "source_trace": str(trace),
               "layers": spans, "files": []}
    encoded_files = []
    for node, (first, end) in enumerate(cuts):
        body = struct.pack("<4Q", STATE_MAGIC, 1, first, end) + b"".join(layers[first:end])
        validate_range(body, past, first, end)
        encoded = HEADER.pack(MAGIC, 1, past, token, first, end, len(body), checksum(body)) + body
        name = f"node{node}.bin"
        receipt["files"].append({"name": name, "first": first, "end": end,
                                 "bytes": len(encoded), "sha256": digest(encoded)})
        encoded_files.append((name, encoded))
    # Validate the whole source before creating any output. Existing fixtures are immutable.
    output.mkdir(parents=True, exist_ok=False)
    for name, encoded in encoded_files:
        (output / name).write_bytes(encoded)
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return receipt


def validate(directory, nodes):
    receipt = json.loads((directory / "receipt.json").read_text())
    cuts = partition(nodes)
    if (receipt["format"] != "w6-deepseek-real-history-v1" or receipt["nodes"] != nodes
            or len(receipt["files"]) != nodes or not 0 < receipt["positions"] < 2**32 - 1
            or not 0 <= receipt["next_token"] < VOCAB):
        raise ValueError("checkpoint receipt geometry differs from cluster")
    for node, ((first, end), entry) in enumerate(zip(cuts, receipt["files"])):
        name = f"node{node}.bin"
        if entry["name"] != name or (entry["first"], entry["end"]) != (first, end):
            raise ValueError("checkpoint partition differs from cluster")
        data = (directory / name).read_bytes()
        if len(data) < HEADER.size:
            raise ValueError("truncated checkpoint envelope")
        magic, version, past, token, lo, hi, size, check = HEADER.unpack_from(data)
        body = data[HEADER.size:]
        if ((magic, version, past, token, lo, hi, size) !=
                (MAGIC, 1, receipt["positions"], receipt["next_token"], first, end, len(body))
                or check != checksum(body) or entry["bytes"] != len(data)
                or entry["sha256"] != digest(data)):
            raise ValueError("checkpoint envelope/checksum mismatch")
        validate_range(body, past, first, end)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("convert")
    build.add_argument("--payload", required=True, type=Path)
    build.add_argument("--trace", required=True, type=Path)
    build.add_argument("--output", required=True, type=Path)
    build.add_argument("--nodes", required=True, type=int)
    check = sub.add_parser("validate")
    check.add_argument("--directory", required=True, type=Path)
    check.add_argument("--nodes", required=True, type=int)
    args = parser.parse_args()
    try:
        if args.command == "convert":
            receipt = convert(args.payload, args.trace, args.output, args.nodes)
        else:
            receipt = validate(args.directory, args.nodes)
        print(json.dumps({"status": "pass", "positions": receipt["positions"],
                          "next_token": receipt["next_token"], "nodes": args.nodes,
                          "native_sha256": receipt["native_sha256"]}))
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        parser.exit(1, f"checkpoint rejected: {error}\n")


if __name__ == "__main__":
    main()
