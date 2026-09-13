#!/usr/bin/env python3
"""Validate the single-object, two-dispatch managed CPU/PTO acceptance trace."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

from validate_mem_service_mapping_resources import validate_kernel_regions


def require(condition, message):
    if not condition:
        raise ValueError(message)


def records(text, prefix):
    result = []
    for line in text.splitlines():
        if not line.startswith(prefix + " "):
            continue
        row = {}
        for token in line[len(prefix) + 1:].split():
            require("=" in token, f"malformed {prefix} row")
            key, value = token.split("=", 1)
            require(key not in row, f"duplicate {prefix} field: {key}")
            row[key] = value
        result.append(row)
    return result


def number(row, key):
    return int(row[key], 0)


def oracle_checksum(vector):
    values = []
    for i in range(64):
        a, b = i % 8 + 1, i % 4 + 1
        c = (a + b + 1) * (a + b + 2)
        values.append((a, b, c, (c + b + 1) * (c + b + 2))[vector])
    checksum = 1469598103934665603
    for byte in struct.pack("<64f", *values):
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{checksum:016x}"


def validate(guests, qemu):
    maps, dispatches = [], []
    for node, text in enumerate(guests):
        require("status=fail" not in text and "resources_retained=1" not in text,
                f"node {node}: guest failure")
        rows = records(text, "MANAGED_PTO")
        mapping = [r for r in rows if "cpu_base" in r]
        require(len(mapping) == 1, f"node {node}: expected one managed mapping")
        mapping = mapping[0]
        require(number(mapping, "node") == node and
                number(mapping, "cpu_base") == number(mapping, "address") > 0 and
                number(mapping, "logical_bytes") == 65536 and
                number(mapping, "generation") > 0 and number(mapping, "mapping_id") > 0,
                f"node {node}: invalid managed identity")
        maps.append(mapping)
        done = records(text, "MANAGED_PTO_RESULT")
        require(len(done) == 1 and done[0]["status"] == "pass" and
                number(done[0], "node") == node and
                done[0]["key"] == mapping["key"] and
                done[0]["generation"] == mapping["generation"] and
                all(done[0][k] == "1" for k in ("mapping_closed", "holder_released")) and
                done[0]["compute_leases"] == "0", f"node {node}: lifetime not drained")
        verified = [r for r in rows if "cpu_verify" in r]
        expected_vectors = [2, 3] if node == 0 else [0, 1, 2, 3]
        require([number(r, "vector") for r in verified] == expected_vectors,
                f"node {node}: missing or repeated CPU checks")
        for row, vector in zip(verified, expected_vectors):
            require(row["cpu_verify"] == "pass" and number(row, "bytes") == 256 and
                    row["checksum"] == oracle_checksum(vector) and
                    number(row, "version") == (1 if vector < 2 else vector),
                    f"node {node}: CPU oracle mismatch")
        for metric in ("put_object_count", "materialize_object_count"):
            require(re.findall(rf"^{metric}=([0-9]+)$", text, re.M) == ["0"],
                    f"node {node}: unexpected control-plane data operation")
        validate_kernel_regions(text)
        if node == 1:
            dispatches = [r for r in rows if "retained_leases" in r]
            completions = [r for r in rows if "completion_op" in r]
            require([number(r, "dispatch") for r in dispatches] == [1, 2] and
                    [number(r, "dispatch") for r in completions] == [1, 2],
                    "expected two ordered dispatches and completions")
            for request, completion, offsets in zip(dispatches, completions,
                    ("3968,8064,12160", "12160,8064,16256")):
                require(request["op_id"] == completion["completion_op"] and
                        request["generation"] == mapping["generation"] and
                        request["mapping_id"] == mapping["mapping_id"] and
                        request["offsets"] == offsets and request["tensor_bytes"] == "256" and
                        request["retained_leases"] == "3" and request["release_guard"] == "pass" and
                        completion["status"] == "1" and completion["error"] == "none",
                        "dispatch identity, geometry, lifetime or completion mismatch")
    for key in ("key", "generation", "cpu_base", "address", "logical_bytes"):
        require(maps[0][key] == maps[1][key], f"CPU mappings differ: {key}")
    require("live_objects=0" in guests[0], "home object not reclaimed")
    requests = [r["op_id"] for r in dispatches]
    require(len(set(requests)) == 2, "reused PTO operation identity")
    validate_callbacks(qemu, maps[0], requests)
    return {"status": "pass", "nodes": 2, "objects": 1, "dispatches": 2,
            "load_callbacks": 4, "store_callbacks": 2, "fences": 2,
            "scope": "single-thread-cross-page", "am2_complete": False}


def validate_callbacks(text, mapping, requests):
    require(not re.search(r"QEMU_UB_GM_.*(?: denied |REJECT |FAILURE_COMPLETION |TIMEOUT )", text),
            "QEMU rejected a managed dispatch or callback")
    bound = records(text, "QEMU_UB_GM_GSVA_BIND")
    require(len(bound) == 6, "strict GSVA binding evidence missing")
    identity_fields = ("segment", "home_va", "epoch", "lease_epoch", "backing_token")
    identities = {tuple(number(row, key) for key in identity_fields) for row in bound}
    require(len(identities) == 1 and all(next(iter(identities))), "PTO backing identity changed")
    require(number(bound[0], "home_va") == number(mapping, "cpu_base"),
            "PTO backing does not match the CPU GSVA address")
    registers = records(text, "SIM_QEMU_UB_GM_BIND_REGISTER")
    unbinds = records(text, "QEMU_UB_GM_UNBIND")
    require([r["request"] for r in registers] == requests and
            [r["request"] for r in unbinds] == requests, "PTO completion sequence differs")
    inputs = records(text, "QEMU_UB_GM_INPUT_AUTHORIZE")
    outputs = records(text, "QEMU_UB_GM_OUTPUT_AUTHORIZE")
    require(len(inputs) == 4 and len(outputs) == 2, "wrong PTO authorization count")
    loads, stores, fences = [records(text, "QEMU_UB_GM_" + kind)
                              for kind in ("LOAD", "STORE", "FENCE")]
    require((len(loads), len(stores), len(fences)) == (4, 2, 2), "wrong callback count")
    bases = []
    for index, request in enumerate(requests):
        select = lambda rows: [r for r in rows if r["request"] == request]
        ins, outs, gsva = select(inputs), select(outputs), select(bound)
        require((len(ins), len(outs), len(gsva)) == (2, 1, 3), "incomplete request bindings")
        auth = ins + outs
        require(len({r["binding"] for r in auth}) == 3 and
                {r["binding"] for r in auth} == {r["binding"] for r in gsva} and
                all(number(r, "length") == 256 for r in auth), "binding identities differ")
        offsets = (3968, 8064, 12160) if index == 0 else (12160, 8064, 16256)
        base = number(auth[0], "local") - offsets[0]
        require(base > 0 and all(number(r, "local") == base + offset
                for r, offset in zip(auth, offsets)), "PTO range geometry mismatch")
        bases.append(base)
        read, write, fence = select(loads), select(stores), select(fences)
        require([r["binding"] for r in read] == [r["binding"] for r in ins] and
                len(write) == len(fence) == 1 and
                write[0]["binding"] == fence[0]["binding"] == outs[0]["binding"] and
                all(number(r, "length") == 256 for r in read + write + fence),
                "callback permissions, range or binding mismatch")
        registered, unbound = registers[index], unbinds[index]
        require(registered["op"] == unbound["op"] == request and
                registered["bindings"] == "3" and unbound["bindings"] == "3" and
                unbound["reason"] == "completion_success" and
                (unbound["load_bytes"], unbound["store_bytes"], unbound["fences"],
                 unbound["segment_payload_staging_bytes"]) == ("512", "256", "1", "0"),
                "PTO completion accounting or zero-staging evidence mismatch")
    require(bases[0] == bases[1], "second dispatch uses a different physical view")
    require(number(outputs[0], "local") == number(inputs[2], "local"),
            "TSTORE output is not the next TLOAD input")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", required=True, type=Path)
    parser.add_argument("--node-b", required=True, type=Path)
    parser.add_argument("--qemu-b", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    paths = [args.node_a, args.node_b, args.qemu_b]
    try:
        contents = [path.read_bytes() for path in paths]
        text = [data.decode("utf-8", errors="replace") for data in contents]
        result = validate(text[:2], text[2])
    except (OSError, ValueError, KeyError, IndexError) as error:
        result = {"status": "fail", "error": str(error), "am2_complete": False}
    else:
        result["sources"] = [{"path": str(path), "sha256": hashlib.sha256(data).hexdigest()}
                             for path, data in zip(paths, contents)]
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
