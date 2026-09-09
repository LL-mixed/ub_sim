#!/usr/bin/env python3
"""Compile and validate the Qwen3 PTO numerical primitives without model weights."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct


ROOT = Path(__file__).resolve().parents[3]
OPERATIONS = ["rms_norm", "linear", "rope", "attention", "softmax", "residual",
              "swiglu", "embedding", "kv_copy", "cast"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--pto-isa-root", type=Path, default=ROOT / "vendor/pto-isa")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "out/qwen3-pto-ops")
    parser.add_argument("--describe", action="store_true")
    args = parser.parse_args()
    if args.describe:
        print(json.dumps({"backend": "pto_cpu", "operations": OPERATIONS,
                          "memory_modes": ["gm", "ub-gm"],
                          "scope": "operator numerical and callback validation; no W5 E2E"}))
        return 0
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "guest-linux/aarch64/tests/qwen3_pto_ops_golden.cpp"
    header_dir = ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto"
    binary = output / "qwen3_pto_ops_golden"
    command = [args.compiler, "-std=c++20", "-O2", "-D__CPU_SIM", "-pthread",
               "-I", str(args.pto_isa_root.resolve() / "include"),
               "-I", str(header_dir), str(source), "-o", str(binary)]
    build = subprocess.run(command, text=True, capture_output=True, timeout=180)
    (output / "build.log").write_text(build.stdout + build.stderr)
    if build.returncode:
        raise SystemExit(f"compile failed ({build.returncode}); see {output / 'build.log'}")
    records = []
    for mode in ("gm", "ub-gm"):
        run = subprocess.run([str(binary), mode], text=True, capture_output=True, timeout=120)
        (output / f"{mode}.log").write_text(run.stdout + run.stderr)
        if run.returncode:
            raise SystemExit(f"{mode} failed ({run.returncode}); see {output / (mode + '.log')}")
        record = json.loads(run.stdout)
        if record["status"] != "pass" or set(record["operations"]) != set(OPERATIONS):
            raise SystemExit(f"incomplete operation evidence for {mode}")
        if record.get("decoder_layer_cases") != ["prefill", "decode_with_past_kv"]:
            raise SystemExit(f"incomplete decoder layer evidence for {mode}")
        if record.get("model_edge_cases") != ["embedding_tokens", "terminal_logits"]:
            raise SystemExit(f"incomplete embedding/logits evidence for {mode}")
        if record.get("range_cases") != ["full_prefill_independent_head", "terminal_decode_with_kv"]:
            raise SystemExit(f"incomplete multi-layer range evidence for {mode}")
        records.append(record)
        trace = output / f"{mode}-trace"
        trace.mkdir(exist_ok=False)
        environment = os.environ.copy()
        environment["SIM_QWEN3_PTO_TRACE_DIR"] = str(trace)
        traced = subprocess.run([str(binary), mode], env=environment, text=True,
                                capture_output=True, timeout=120)
        (output / f"{mode}-trace.log").write_text(traced.stdout + traced.stderr)
        if traced.returncode:
            raise SystemExit(f"{mode} numerical trace failed")
        files = sorted(trace.glob("*.bin"))
        if len(files) != 2:
            raise SystemExit("missing range snapshots")
        for path in files:
            data = path.read_bytes()
            if data[:8] != b"QPTOTR1\0":
                raise SystemExit("invalid trace magic")
            first, end, past, tokens, hidden, kv_width, vocab = struct.unpack_from("<7I", data, 8)
            cursor, kinds, logit_cols = 36, [], 0
            while True:
                kind, layer, row, col, rows, cols = struct.unpack_from("<6I", data, cursor)
                cursor += 24
                if kind == 0:
                    if any((layer, row, col, rows, cols)) or cursor != len(data):
                        raise SystemExit("invalid trace terminator")
                    break
                if kind == 4:
                    if layer != end or row != 0 or rows != 1 or col != logit_cols:
                        raise SystemExit("invalid logits trace geometry")
                    logit_cols += cols
                else:
                    want_rows = tokens if kind == 1 else past + tokens
                    want_cols = hidden if kind == 1 else kv_width
                    if (rows, cols, row, col) != (want_rows, want_cols, 0, 0):
                        raise SystemExit("invalid layer trace geometry")
                    kinds.append((kind, layer))
                cursor += rows * cols * 4
                if cursor > len(data):
                    raise SystemExit("truncated trace")
            if kinds != [(kind, layer) for layer in range(first, end) for kind in (1, 2, 3)] or logit_cols != vocab:
                raise SystemExit("incomplete numerical trace")
        # Evidence must never silently replace an earlier run.
        repeated = subprocess.run([str(binary), mode], env=environment, text=True,
                                  capture_output=True, timeout=120)
        if repeated.returncode == 0 or "qwen3_pto_trace_open_failed" not in repeated.stderr:
            raise SystemExit("trace overwrite was not rejected")
    report = {"status": "pass", "scope": "operator-only", "command": command,
              "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "header_sha256": hashlib.sha256((header_dir / "qwen3_pto_ops.hpp").read_bytes()).hexdigest(),
              "layer_sha256": hashlib.sha256((header_dir / "qwen3_pto_layer.hpp").read_bytes()).hexdigest(),
              "range_sha256": hashlib.sha256((header_dir / "qwen3_pto_range.hpp").read_bytes()).hexdigest(),
              "trace_sha256": hashlib.sha256((header_dir / "qwen3_pto_trace.hpp").read_bytes()).hexdigest(),
              "trace_validation": "pass: geometry, completeness, numerical golden and no overwrite",
              "results": records}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
