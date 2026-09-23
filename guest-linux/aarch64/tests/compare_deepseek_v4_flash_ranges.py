#!/usr/bin/env python3
"""Compare two raw little-endian f32 DeepSeek range outputs."""

import argparse
import json
import math
from pathlib import Path
import struct
from typing import Optional


def read_f32(path: Path, offset_bytes: int = 0, elements: Optional[int] = None) -> list[float]:
    payload = path.read_bytes()
    if (offset_bytes < 0 or offset_bytes % 4 or offset_bytes > len(payload)
            or elements is not None and elements <= 0):
        raise ValueError(f"invalid f32 slice: {path}")
    end = len(payload) if elements is None else offset_bytes + elements * 4
    if end > len(payload):
        raise ValueError(f"f32 slice outside payload: {path}")
    payload = payload[offset_bytes:end]
    if not payload or len(payload) % 4:
        raise ValueError(f"invalid f32 payload: {path}")
    return [value for (value,) in struct.iter_unpack("<f", payload)]


def compare(reference: list[float], candidate: list[float], atol: float, rtol: float) -> dict:
    if len(reference) != len(candidate):
        raise ValueError(f"tensor length mismatch: {len(reference)} != {len(candidate)}")
    if not math.isfinite(atol) or not math.isfinite(rtol) or atol < 0 or rtol < 0:
        raise ValueError("tolerances must be finite and nonnegative")
    if any(not math.isfinite(value) for value in reference + candidate):
        raise ValueError("tensor contains non-finite values")

    count = len(reference)
    errors = [abs(left - right) for left, right in zip(reference, candidate)]
    worst = max(range(count), key=errors.__getitem__)
    squared_error = sum(error * error for error in errors)
    reference_norm_sq = sum(value * value for value in reference)
    candidate_norm_sq = sum(value * value for value in candidate)
    dot = sum(left * right for left, right in zip(reference, candidate))
    failed = sum(
        error > atol + rtol * abs(value)
        for value, error in zip(reference, errors)
    )
    return {
        "elements": count,
        "exact_elements": sum(left == right for left, right in zip(reference, candidate)),
        "atol": atol,
        "rtol": rtol,
        "outside_tolerance": failed,
        "max_abs_error": errors[worst],
        "max_abs_index": worst,
        "reference_at_max": reference[worst],
        "candidate_at_max": candidate[worst],
        "mean_abs_error": sum(errors) / count,
        "rmse": math.sqrt(squared_error / count),
        "relative_l2_error": (
            math.sqrt(squared_error / reference_norm_sq)
            if reference_norm_sq else None
        ),
        "cosine_similarity": (
            dot / math.sqrt(reference_norm_sq * candidate_norm_sq)
            if reference_norm_sq and candidate_norm_sq else None
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--atol", type=float, required=True)
    parser.add_argument("--rtol", type=float, required=True)
    parser.add_argument("--offset-bytes", type=int, default=0)
    parser.add_argument("--elements", type=int)
    args = parser.parse_args()
    result = compare(
        read_f32(args.reference, args.offset_bytes, args.elements),
        read_f32(args.candidate, args.offset_bytes, args.elements),
        args.atol,
        args.rtol,
    )
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
