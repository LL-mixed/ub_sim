#!/usr/bin/env python3
"""Validate one eight-node Lingqu shmem PTO UB_GM functional demo."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


NODE_NAMES = ("nodeA", "nodeB", "nodeC", "nodeD", "nodeE", "nodeF", "nodeG", "nodeH")
CONSUMER_COUNT = 7
ELEMENTS = 16_384
TENSOR_BYTES = ELEMENTS * 4
LANE_STRIDE = TENSOR_BYTES * 3
INPUT_A_OFFSET = 0
INPUT_B_OFFSET = TENSOR_BYTES
OUTPUT_OFFSET = TENSOR_BYTES * 2
COMPLETION_ACK_BASE = CONSUMER_COUNT * LANE_STRIDE
COMPLETION_ACK_MAGIC = 0x4C51_5054_4F41_434B
FORBIDDEN_FEATURE_PATTERNS = (
    "UB_NPU: created",
    "SIM_DEC: GVA_MAP",
    "GVA_S3_MAP",
    "GVA_ROUTE_DUMP",
    "GSVA_MODE",
    "GSVA_",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--logs-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--artifact-fingerprint", required=True)
    parser.add_argument("--cna-base", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--root-commit", required=True)
    return parser.parse_args()


def read_log(path: Path, errors: list[str]) -> str:
    if not path.is_file():
        errors.append(f"missing log: {path.name}")
        return ""
    return path.read_text(encoding="utf-8", errors="replace").replace("\r", "")


def marker_fields(text: str, required_tokens: tuple[str, ...]) -> list[dict[str, str]]:
    matches: list[dict[str, str]] = []
    for line in text.splitlines():
        if not all(token in line for token in required_tokens):
            continue
        fields: dict[str, str] = {}
        for token in line.split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        matches.append(fields)
    return matches


def require_one(
    text: str,
    required_tokens: tuple[str, ...],
    label: str,
    errors: list[str],
) -> dict[str, str]:
    matches = marker_fields(text, required_tokens)
    if len(matches) != 1:
        errors.append(f"{label}: expected 1 marker, found {len(matches)}")
        return {}
    return matches[0]


def parse_int_field(
    fields: dict[str, str], key: str, label: str, errors: list[str]
) -> int | None:
    value = fields.get(key)
    if value is None:
        errors.append(f"{label}: missing {key}")
        return None
    try:
        return int(value, 0)
    except ValueError:
        errors.append(f"{label}: invalid {key}={value}")
        return None


def expect_equal(actual: object, expected: object, label: str, errors: list[str]) -> None:
    if actual != expected:
        errors.append(f"{label}: expected {expected!r}, found {actual!r}")


def validate_consumer(
    node_name: str,
    node_id: int,
    guest_text: str,
    qemu_text: str,
    fingerprint: int,
    cna_base: int,
    errors: list[str],
) -> dict[str, object]:
    label = f"{node_name}/node-{node_id}"
    lane_index = node_id - 1
    lane_base = lane_index * LANE_STRIDE
    expected_cna = cna_base + node_id
    generation = int(require_one(
        guest_text,
        ("LINGQU_SHMEM_PTO role=consumer stage=start", f"node_id={node_id}"),
        f"{label} start",
        errors,
    ).get("generation", "0"), 0)
    expected_op_id = (generation << 16) | (lane_index << 8) | 1
    expected_request_id = (expected_op_id & ~0xFF) | 2
    expected_ack_offset = COMPLETION_ACK_BASE + lane_index * 8
    expected_ack_marker = COMPLETION_ACK_MAGIC ^ (generation << 8) ^ node_id

    prepared = require_one(
        guest_text,
        ("LINGQU_SHMEM_PTO role=consumer stage=prepared", f"node_id={node_id}"),
        f"{label} prepared",
        errors,
    )
    result = require_one(
        guest_text,
        ("LINGQU_SHMEM_PTO_RESULT role=consumer status=pass", f"node_id={node_id}"),
        f"{label} result",
        errors,
    )
    completion_ack = require_one(
        guest_text,
        (
            "LINGQU_SHMEM_PTO role=consumer stage=completion_ack_published",
            f"node_id={node_id}",
        ),
        f"{label} completion acknowledgement",
        errors,
    )
    local_pa = parse_int_field(prepared, "local_pa", label, errors)
    checks = {
        "node_id": parse_int_field(prepared, "node_id", label, errors),
        "lane_index": parse_int_field(prepared, "lane_index", label, errors),
        "lane_base": parse_int_field(prepared, "lane_base", label, errors),
        "requester_cna": parse_int_field(prepared, "requester_cna", label, errors),
        "artifact_fingerprint": parse_int_field(prepared, "fingerprint", label, errors),
        "op_id": parse_int_field(prepared, "op_id", label, errors),
        "request_id": parse_int_field(prepared, "request_id", label, errors),
        "result_op_id": parse_int_field(result, "op_id", label, errors),
        "result_request_id": parse_int_field(result, "request_id", label, errors),
        "ack_offset": parse_int_field(
            completion_ack, "ack_offset", label, errors
        ),
        "ack_marker": parse_int_field(completion_ack, "marker", label, errors),
    }
    expected_checks = {
        "node_id": node_id,
        "lane_index": lane_index,
        "lane_base": lane_base,
        "requester_cna": expected_cna,
        "artifact_fingerprint": fingerprint,
        "op_id": expected_op_id,
        "request_id": expected_request_id,
        "result_op_id": expected_op_id,
        "result_request_id": expected_request_id,
        "ack_offset": expected_ack_offset,
        "ack_marker": expected_ack_marker,
    }
    for key, expected in expected_checks.items():
        expect_equal(checks[key], expected, f"{label} {key}", errors)

    access_pattern = re.compile(
        r"QEMU_UB_GM_(LOAD|STORE) request=.* addr=0x([0-9a-f]+) "
        r"length=([0-9]+) "
    )
    accesses = [
        (match.group(1), int(match.group(2), 16), int(match.group(3)))
        for match in access_pattern.finditer(qemu_text)
    ]
    expected_accesses: list[tuple[str, int, int]] = []
    if local_pa is not None:
        expected_accesses = [
            ("LOAD", local_pa + lane_base + INPUT_A_OFFSET, TENSOR_BYTES),
            ("LOAD", local_pa + lane_base + INPUT_B_OFFSET, TENSOR_BYTES),
            ("STORE", local_pa + lane_base + OUTPUT_OFFSET, TENSOR_BYTES),
        ]
        expect_equal(accesses, expected_accesses, f"{label} data callbacks", errors)

    fence_pattern = re.compile(
        rf"QEMU_UB_GM_FENCE request=.* length={TENSOR_BYTES}(?:\s|$)"
    )
    fence_count = len(fence_pattern.findall(qemu_text))
    expect_equal(fence_count, 1, f"{label} fence count", errors)
    expect_equal(
        qemu_text.count(f"QEMU_UB_GM_ACCESS_REGISTER pto_device_cna=0x{expected_cna:x}"),
        1,
        f"{label} PTO CNA registration",
        errors,
    )
    if not re.search(
        rf"SIM_QEMU_UB_GM_BIND_REGISTER .*bindings=3 requester_cna=0x{expected_cna:x}(?:\s|$)",
        qemu_text,
    ):
        errors.append(f"{label}: missing three-memref binding")
    if not re.search(
        rf"QEMU_UB_GM_UNBIND .*reason=completion_success bindings=3 "
        rf"load_bytes={TENSOR_BYTES * 2} store_bytes={TENSOR_BYTES} "
        rf"fences=1 segment_payload_staging_bytes=0(?:\s|$)",
        qemu_text,
    ):
        errors.append(f"{label}: missing zero-staging completion unbind")
    if "QEMU_UB_GM_DISPATCH_REJECT" in qemu_text:
        errors.append(f"{label}: dispatch rejected")
    for forbidden in FORBIDDEN_FEATURE_PATTERNS:
        if forbidden in qemu_text:
            errors.append(f"{label}: experimental feature leaked: {forbidden}")

    return {
        "node": node_name,
        "node_id": node_id,
        "lane_index": lane_index,
        "lane_base": lane_base,
        "requester_cna": f"0x{expected_cna:x}",
        "op_id": expected_op_id,
        "request_id": expected_request_id,
        "load_calls": sum(kind == "LOAD" for kind, _, _ in accesses),
        "load_bytes": sum(length for kind, _, length in accesses if kind == "LOAD"),
        "store_calls": sum(kind == "STORE" for kind, _, _ in accesses),
        "store_bytes": sum(length for kind, _, length in accesses if kind == "STORE"),
        "fences": fence_count,
        "segment_payload_staging_bytes": 0,
        "completion_ack_offset": expected_ack_offset,
        "completion_ack_marker": f"0x{expected_ack_marker:016x}",
        "expected_accesses": [
            {"kind": kind, "address": f"0x{address:x}", "length": length}
            for kind, address, length in expected_accesses
        ],
    }


def main() -> int:
    args = parse_args()
    errors: list[str] = []
    fingerprint = int(args.artifact_fingerprint, 0)
    producer_guest = read_log(args.logs_dir / "nodeA_guest.log", errors)
    producer_qemu = read_log(args.logs_dir / "nodeA_qemu.log", errors)

    producer_result = require_one(
        producer_guest,
        (
            "LINGQU_SHMEM_PTO_RESULT role=producer status=pass",
            "consumers=7",
            "lanes_verified=7",
            "completion_acks=7",
        ),
        "producer result",
        errors,
    )
    producer_verify = require_one(
        producer_guest,
        ("LINGQU_SHMEM_PTO role=producer producer_verify=pass", "consumers=7"),
        "producer oracle",
        errors,
    )
    producer_acks = require_one(
        producer_guest,
        (
            "LINGQU_SHMEM_PTO role=producer stage=completion_acks_verified",
            "consumers=7",
            f"ack_base={COMPLETION_ACK_BASE}",
        ),
        "producer completion acknowledgements",
        errors,
    )
    if "QEMU_UB_GM_LOAD " in producer_qemu or "QEMU_UB_GM_STORE " in producer_qemu:
        errors.append("producer QEMU executed PTO data callbacks")
    for forbidden in FORBIDDEN_FEATURE_PATTERNS:
        if forbidden in producer_qemu:
            errors.append(f"producer: experimental feature leaked: {forbidden}")

    consumers: list[dict[str, object]] = []
    for node_id, node_name in enumerate(NODE_NAMES[1:], start=1):
        guest_text = read_log(args.logs_dir / f"{node_name}_guest.log", errors)
        qemu_text = read_log(args.logs_dir / f"{node_name}_qemu.log", errors)
        consumers.append(
            validate_consumer(
                node_name,
                node_id,
                guest_text,
                qemu_text,
                fingerprint,
                args.cna_base,
                errors,
            )
        )

    unique_lanes = {entry["lane_index"] for entry in consumers}
    unique_cnas = {entry["requester_cna"] for entry in consumers}
    unique_ops = {entry["op_id"] for entry in consumers}
    unique_requests = {entry["request_id"] for entry in consumers}
    expect_equal(len(unique_lanes), CONSUMER_COUNT, "unique lanes", errors)
    expect_equal(len(unique_cnas), CONSUMER_COUNT, "unique requester CNAs", errors)
    expect_equal(len(unique_ops), CONSUMER_COUNT, "unique op IDs", errors)
    expect_equal(len(unique_requests), CONSUMER_COUNT, "unique request IDs", errors)

    totals = {
        "consumer_count": len(consumers),
        "load_calls": sum(int(entry["load_calls"]) for entry in consumers),
        "load_bytes": sum(int(entry["load_bytes"]) for entry in consumers),
        "store_calls": sum(int(entry["store_calls"]) for entry in consumers),
        "store_bytes": sum(int(entry["store_bytes"]) for entry in consumers),
        "fences": sum(int(entry["fences"]) for entry in consumers),
        "segment_payload_staging_bytes": 0,
    }
    expected_totals = {
        "consumer_count": 7,
        "load_calls": 14,
        "load_bytes": 917_504,
        "store_calls": 7,
        "store_bytes": 458_752,
        "fences": 7,
        "segment_payload_staging_bytes": 0,
    }
    expect_equal(totals, expected_totals, "aggregate callbacks", errors)

    report = {
        "schema": "lingqu-shmem-pto-eight-node-validation-v1",
        "run_id": args.run_id,
        "validation": {"status": "pass" if not errors else "fail", "errors": errors},
        "root_commit": args.root_commit,
        "artifact_fingerprint": f"0x{fingerprint:016x}",
        "topology": {
            "nodes": 8,
            "producer": "nodeA",
            "consumers": list(NODE_NAMES[1:]),
            "layout": "nd",
            "elements_per_consumer": ELEMENTS,
        },
        "producer": {
            "oracle": "pass" if producer_verify else "missing",
            "lanes_verified": int(producer_result.get("lanes_verified", "0"), 0)
            if producer_result
            else 0,
            "completion_acks": int(producer_result.get("completion_acks", "0"), 0)
            if producer_result and producer_acks
            else 0,
            "completion_ack_base": COMPLETION_ACK_BASE,
        },
        "consumers": consumers,
        "totals": totals,
        "execution_boundary": {
            "functional_demo": True,
            "host_callable_parallelism_proven": False,
            "host_dispatch_serialization": "possible",
            "host_dispatch_lock": "/tmp/linqu_simpler_host_vector.lock",
            "statement": (
                "All seven QEMU consumers complete the PTO callable. The host-wide "
                "dispatch lock may serialize Simpler callable execution on one host."
            ),
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
