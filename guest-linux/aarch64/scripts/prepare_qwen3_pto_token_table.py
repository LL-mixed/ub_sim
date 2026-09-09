#!/usr/bin/env python3
"""Prepare fixed-width tokenizer control metadata for W5 PTO guest sampling."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def token_table(weights):
    config = json.loads((weights / "config.json").read_text())
    tokenizer = json.loads((weights / "tokenizer.json").read_text())
    vocab = config["vocab_size"]
    if not isinstance(vocab, int) or not 4 <= vocab <= 1 << 24:
        raise ValueError("invalid configured vocabulary")
    pieces = {value: key for key, value in tokenizer["model"]["vocab"].items()}
    for item in tokenizer.get("added_tokens", []):
        pieces.setdefault(item["id"], item["content"])
    fallback = weights / "vocab.json"
    if fallback.exists():
        for piece, token in json.loads(fallback.read_text()).items():
            pieces.setdefault(token, piece)
    extra = weights / "tokenizer_config.json"
    if extra.exists():
        for token, item in json.loads(extra.read_text()).get("added_tokens_decoder", {}).items():
            pieces.setdefault(int(token), item["content"])
    output = bytearray(struct.pack("<4Q", 0x515750544F545854, 1, vocab, 32))
    for token in range(vocab):
        if token not in pieces:
            output.extend(bytes(32))
            continue
        piece = pieces[token].encode("utf-8")
        checksum = 0xCBF29CE484222325 ^ token
        for byte in piece:
            checksum = ((checksum ^ byte) * 0x100000001B3) & ((1 << 64) - 1)
        size = len(piece)
        checksum ^= ((size << 17) | (size >> 47)) & ((1 << 64) - 1)
        output.extend(struct.pack("<2Q", checksum, size) + piece[:16].ljust(16, b"\0"))
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = token_table(args.weights)
    with args.output.open("xb") as stream:
        stream.write(data)
    print(json.dumps({"status": "pass", "bytes": len(data),
                      "sha256": hashlib.sha256(data).hexdigest(),
                      "scope": "tokenizer metadata only; no model execution"}))


if __name__ == "__main__":
    main()
