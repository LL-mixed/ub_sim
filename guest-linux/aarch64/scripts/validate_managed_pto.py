#!/usr/bin/env python3
"""Validate managed CPU/PTO geometry, ownership, data and completion traces."""
import argparse
import functools
import hashlib
import json
import re
import struct
from pathlib import Path

from validate_mem_service_mapping_resources import validate_kernel_regions


GEOMETRIES = {
    "cross-page": (64, 65536, (3968, 8064, 12160, 16256)),
    "nd": (16384, 262144, (0, 65536, 131072, 196608)),
    "tail": (16256, 262144, (0, 65024, 130048, 195072)),
}


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


def checksum64(data):
    checksum = 1469598103934665603
    for byte in data:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{checksum:016x}"


@functools.lru_cache(maxsize=12)
def oracle_checksum(vector, elements=64):
    values = []
    for i in range(elements):
        a, b = i % 8 + 1, i % 4 + 1
        if i >= 64:
            a, b = i // 8 + 1, i % 8 + 1
        c = (a + b + 1) * (a + b + 2)
        values.append((a, b, c, (c + b + 1) * (c + b + 2))[vector])
    return checksum64(struct.pack(f"<{elements}f", *values))


def validate_backing_identities(bound, mappings):
    fields = ("segment", "home_va", "epoch", "lease_epoch", "backing_token")
    expected = {number(mapping, "cpu_base") for mapping in mappings}
    identities = {}
    for row in bound:
        identity = tuple(number(row, key) for key in fields)
        home = number(row, "home_va")
        require(all(identity) and home in expected, "PTO backing does not match a CPU object")
        require(home not in identities or identities[home] == identity,
                "PTO backing identity changed")
        identities[home] = identity
    require(set(identities) == expected, "missing object backing identity")


def validate(guests, qemu, require_timeout_recovery=False, access_threads=None,
             require_negative_checks=False, require_readonly_inputs=False, layout="cross-page"):
    require(layout in GEOMETRIES, "unsupported managed geometry")
    elements, object_bytes, offsets = GEOMETRIES[layout]
    tensor_bytes = elements * 4
    # Serial logs use CRLF; normalize only parsing, never the evidence bytes.
    guests = [text.replace("\r\n", "\n") for text in guests]
    maps, output_maps, dispatches = [], [], []
    for node, text in enumerate(guests):
        require("status=fail" not in text and "resources_retained=1" not in text,
                f"node {node}: guest failure")
        rows = records(text, "MANAGED_PTO")
        mappings = [r for r in rows if "cpu_base" in r]
        require(len(mappings) == (2 if require_readonly_inputs else 1),
                f"node {node}: wrong managed mapping count")
        if require_readonly_inputs:
            require([r.get("object") for r in mappings] == ["input", "output"],
                    f"node {node}: missing input/output mapping identities")
        mapping, output = mappings[0], mappings[-1]
        for index, record in enumerate(mappings):
            require(number(record, "node") == node and
                    number(record, "cpu_base") == number(record, "address") > 0 and
                    number(record, "logical_bytes") == object_bytes and
                    number(record, "generation") > 0 and number(record, "mapping_id") > 0,
                    f"node {node}: invalid managed identity")
            expected_access = 1 if require_readonly_inputs and node == 1 and index == 0 else 3
            require(int(record.get("access", "3"), 0) == expected_access,
                    f"node {node}: incorrect mapping permissions")
        if require_readonly_inputs:
            require(output["key"] == mapping["key"] + "-output" and
                    number(output, "cpu_base") != number(mapping, "cpu_base") and
                    number(output, "mapping_id") != number(mapping, "mapping_id"),
                    "input and output must be distinct managed objects")
        maps.append(mapping)
        output_maps.append(output)
        done = records(text, "MANAGED_PTO_RESULT")
        require(len(done) == 1 and done[0]["status"] == "pass" and
                number(done[0], "node") == node and
                done[0]["key"] == mapping["key"] and
                done[0]["generation"] == mapping["generation"] and
                all(done[0][k] == "1" for k in ("mapping_closed", "holder_released")) and
                done[0]["compute_leases"] == "0", f"node {node}: lifetime not drained")
        require(done[0].get("objects", "1") == ("2" if require_readonly_inputs else "1") and
                done[0].get("readonly_inputs", "0") == ("1" if require_readonly_inputs else "0"),
                "missing or unexpected read-only object mode")
        verified = [r for r in rows if "cpu_verify" in r]
        expected_vectors = [2, 3] if node == 0 else [0, 1, 2, 3]
        require([number(r, "vector") for r in verified] == expected_vectors,
                f"node {node}: missing or repeated CPU checks")
        for row, vector in zip(verified, expected_vectors):
            require(row["cpu_verify"] == "pass" and number(row, "bytes") == tensor_bytes and
                    row["checksum"] == oracle_checksum(vector, elements) and
                    number(row, "version") == (1 if vector < 2 else vector),
                    f"node {node}: CPU oracle mismatch")
            if require_readonly_inputs:
                require(row.get("object") == ("input" if vector < 2 else "output"),
                        "CPU verified the wrong object")
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
            for request, completion, views in zip(dispatches, completions,
                    ((0, 1, 2), (2, 1, 3))):
                require(request["op_id"] == completion["completion_op"] and
                        request["generation"] == mapping["generation"] and
                        request["mapping_id"] == mapping["mapping_id"] and
                        request["offsets"] == ",".join(str(offsets[v]) for v in views) and
                        number(request, "tensor_bytes") == tensor_bytes and
                        request["retained_leases"] == "3" and request["release_guard"] == "pass" and
                        completion["status"] == "1" and completion["error"] == "none",
                        "dispatch identity, geometry, lifetime or completion mismatch")
                if require_readonly_inputs:
                    first = mapping if request["dispatch"] == "1" else output
                    expected_ids = ",".join(r["mapping_id"] for r in (first, mapping, output))
                    require(request.get("object_maps") == expected_ids,
                            "dispatch did not borrow the expected object mappings")
    for mapping_pair in (maps, output_maps):
        for key in ("key", "generation", "cpu_base", "address", "logical_bytes"):
            require(mapping_pair[0][key] == mapping_pair[1][key], f"CPU mappings differ: {key}")
    require("live_objects=0" in guests[0], "home object not reclaimed")
    requests = [r["op_id"] for r in dispatches]
    require(len(set(requests)) == 2, "reused PTO operation identity")
    require(not records(guests[0], "MANAGED_PTO_WAIT"), "unexpected home PTO wait")
    waits = records(guests[1], "MANAGED_PTO_WAIT")
    require(len(waits) == (4 if require_timeout_recovery else 0),
            "missing or unexpected timeout recovery evidence")
    for index, row in enumerate(waits):
        request = requests[index // 2]
        require(row["node"] == "1" and number(row, "dispatch") == index // 2 + 1 and
                row["op_id"] == request, "timeout recovery operation mismatch")
        if index % 2 == 0:
            require(row["state"] == "timeout" and row["retained_leases"] == "3" and
                    row["close_guard"] == row["release_guard"] == "pass",
                    "timeout did not retain the original resources")
        else:
            require(row["state"] == "recovered" and row["completion_op"] == request and
                    row["pending_op"] == "0", "original completion was not recovered")
    events = []
    for line in guests[1].splitlines():
        if line.startswith("MANAGED_PTO_WAIT "):
            row = records(line, "MANAGED_PTO_WAIT")[0]
            events.append((row["state"], row["dispatch"], row["op_id"]))
        elif line.startswith("MANAGED_PTO "):
            row = records(line, "MANAGED_PTO")[0]
            if "retained_leases" in row:
                events.append(("submit", row["dispatch"], row["op_id"]))
            elif "completion_op" in row:
                events.append(("completion", row["dispatch"], row["completion_op"]))
    phases = ("submit", "timeout", "recovered", "completion") if require_timeout_recovery else (
        "submit", "completion")
    require(events == [(phase, str(index + 1), request)
                       for index, request in enumerate(requests) for phase in phases],
            "PTO submission, wait and completion order mismatch")
    require(not (require_negative_checks and require_timeout_recovery),
            "negative checks cannot use short-wait recovery")
    output = output_maps[1] if require_readonly_inputs else None
    geometry = GEOMETRIES[layout]
    filtered = validate_negatives(guests, qemu, maps[1], requests, require_negative_checks, output, geometry)
    validate_callbacks(filtered, maps[0], requests, output, geometry)
    validate_readonly_inputs(guests, maps, output_maps, requests, require_readonly_inputs, geometry)
    transport = validate_transport_chunks(qemu, requests, geometry) if layout != "cross-page" else None
    if access_threads is not None:
        if require_readonly_inputs:
            validate_readonly_owners(guests, maps, output_maps, requests,
                                     require_timeout_recovery, access_threads)
        else:
            validate_owners(guests, maps, requests, require_timeout_recovery, access_threads)
    require(not require_readonly_inputs or access_threads is not None,
            "read-only inputs require an explicit owner/thread gate")
    scope = ("multi-thread-" if access_threads else "single-thread-") + layout
    return {"status": "pass", "nodes": 2, "objects": 2 if require_readonly_inputs else 1,
            "dispatches": 8 if require_negative_checks else 2, "successful_dispatches": 2,
            "expected_rejections": 6 if require_negative_checks else 0,
            "execution_rejections": 2 if require_negative_checks else 0,
            "load_callbacks": 8 if require_negative_checks else 4,
            "store_callbacks": 2, "fences": 2,
            "scope": scope + ("-readonly-inputs" if require_readonly_inputs else ""),
            "readonly_inputs_verified": require_readonly_inputs,
            "owner_verified": access_threads is not None, "access_threads": access_threads,
            "timeout_recoveries": len(waits) // 2,
            "layout": layout, "elements": elements, "tensor_bytes": tensor_bytes,
            "object_bytes": object_bytes, "transport_chunks": transport,
            "am2_complete": False}


def validate_negatives(guests, qemu, mapping, requests, required, output_mapping=None,
                       geometry=GEOMETRIES["cross-page"]):
    elements, _, offsets = geometry
    tensor_bytes = elements * 4
    prefixes = ("MANAGED_PTO_NEGATIVE", "MANAGED_PTO_NEGATIVE_DATA")
    require(all(not records(guests[0], prefix) for prefix in prefixes),
            "producer cannot submit negative probes")
    probes, data = [records(guests[1], prefix) for prefix in prefixes]
    if not required:
        require(not probes and not data, "unexpected negative probe evidence")
        return qemu
    names = ("bad-mapping-ref", "wrong-requester", "oob", "role-access-mismatch",
             "tstore-on-read", "tload-on-write")
    require(len(probes) == 12 and len(data) == 24, "negative probe evidence incomplete")
    operations = [str(int(requests[0]) + 2 + index) for index in range(6)]
    errors = ["pto_ub_gm_access_denied" if index in (1, 4, 5) else "pto_ub_gm_bad_memref"
              for index in range(6)]
    expected_events = []
    for index, (name, operation, error) in enumerate(zip(names, operations, errors)):
        submitted, rejected = probes[2*index:2*index+2]
        require(submitted["state"] == "submitted" and rejected["state"] == "rejected" and
                all(row["case"] == name and row["op_id"] == operation and row["node"] == "1"
                    for row in (submitted, rejected)), "negative probe identity/order mismatch")
        require(submitted["mapping_id"] == mapping["mapping_id"] and
                submitted["retained_leases"] == "3" and submitted["expected"] == error and
                0 <= number(submitted, "metadata_crc") <= 0xffffffff and
                rejected["completion_op"] == operation and rejected["status"] == "3" and
                rejected["error"] == error and rejected["pending_op"] == "0",
                "negative completion or retained mapping mismatch")
        expected_events += [(name, "submitted", operation), (name, "rejected", operation)]
        for vector, row in enumerate(data[4*index:4*index+4]):
            expected = oracle_checksum(vector, elements) if vector < 2 else checksum64(
                struct.pack(f"<{elements}I", *([0x7fc00001] * elements)))
            require(row["node"] == "1" and row["case"] == name and row["op_id"] == operation and
                    number(row, "vector") == vector and row["checksum"] == expected and
                    number(row, "bytes") == tensor_bytes and row["unchanged"] == "pass",
                    "negative probe changed original payload")
            if output_mapping is not None:
                require(row.get("object") == ("input" if vector < 2 else "output"),
                        "negative probe checked the wrong object")
            expected_events.append((name, str(vector), operation))
    lines = guests[1].splitlines()
    events, positions = [], []
    for position, line in enumerate(lines):
        for prefix in prefixes:
            rows = records(line, prefix)
            if rows:
                row = rows[0]
                events.append((row["case"], row.get("state", row.get("vector")), row["op_id"]))
                positions.append(position)
    first_positive = next(i for i, line in enumerate(lines)
                          if line.startswith("MANAGED_PTO node=1 dispatch=1 op_id="))
    mapping_position = next(i for i, line in enumerate(lines)
                            if line.startswith("MANAGED_PTO ") and "cpu_base=" in line)
    require(events == expected_events and mapping_position < positions[0] and
            positions[-1] < first_positive, "negative probes must finish before normal compute")
    failure_prefixes = ("QEMU_UB_GM_FAILURE_COMPLETION", "QEMU_UB_GM_DISPATCH_REJECT")
    for prefix in failure_prefixes:
        rows = records(qemu, prefix)
        require([row["op"] for row in rows] == operations[:4] and
                [row["code"] for row in rows] == errors[:4] and
                (prefix != failure_prefixes[0] or all(row["status"] == "3" for row in rows)),
                "QEMU negative completion evidence mismatch")
    filtered = "\n".join(line for line in qemu.splitlines()
                         if not any(line.startswith(prefix + " ") for prefix in failure_prefixes))
    return validate_execution_negatives(filtered, mapping, operations[4:], requests[0],
                                        output_mapping, geometry)


def validate_execution_negatives(text, mapping, operations, positive_request, output_mapping=None,
                                 geometry=GEOMETRIES["cross-page"]):
    elements, _, offsets = geometry
    tensor_bytes = elements * 4
    # Remove only fully validated execution records from this parsing copy.
    # Raw logs stay unchanged; all other callbacks/errors reach the normal gate.
    prefixes = ("QEMU_UB_GM_INPUT_AUTHORIZE", "QEMU_UB_GM_OUTPUT_AUTHORIZE",
                "QEMU_UB_GM_GSVA_BIND", "SIM_QEMU_UB_GM_BIND_REGISTER",
                "QEMU_UB_GM_LOAD", "QEMU_UB_GM_STORE", "QEMU_UB_GM_FENCE",
                "QEMU_UB_GM_UNBIND")
    bound = records(text, prefixes[2])
    validate_backing_identities(bound, [mapping, output_mapping or mapping])
    positive = [row for row in records(text, prefixes[0])
                if row["request"] == positive_request]
    require(len(positive) == 2, "missing recovery input authorizations")
    positive_output = [row for row in records(text, prefixes[1])
                       if row["request"] == positive_request]
    require(len(positive_output) == 1, "missing recovery output authorization")
    bases = [number(row, "local") - offset for row, offset in
             zip(positive + positive_output, offsets[:3])]
    homes = [number(row, "cpu_base") for row in (mapping, mapping, output_mapping or mapping)]
    lines, consumed, ordered_positions = text.splitlines(), set(), []
    for operation in operations:
        selected = []
        for position, line in enumerate(lines):
            if not line.startswith(prefixes):
                continue
            for prefix in prefixes:
                rows = records(line, prefix)
                if rows and rows[0]["request"] == operation:
                    selected.append((prefix, rows[0]))
                    consumed.add(position)
                    ordered_positions.append(position)
        roles = ("INPUT", "INPUT", "OUTPUT")
        expected = []
        for role in roles:
            expected += [f"QEMU_UB_GM_{role}_AUTHORIZE", prefixes[2]]
        expected += [prefixes[3]] + [prefixes[4]] * 2 + [prefixes[7]]
        require([prefix for prefix, _ in selected] == expected,
                "negative execution stage or callback sequence mismatch")
        auth = [selected[2*i][1] for i in range(3)]
        for binding, (row, offset) in enumerate(zip(auth, offsets[:3]), 1):
            require(row["op"] == operation and number(row, "binding") == binding and
                    number(row, "local") == bases[binding-1] + offset and
                    number(row, "length") == tensor_bytes and
                    row["requester_cna"] == positive[0]["requester_cna"] and
                    number(row, "map") > 0 and number(row, "generation") > 0 and
                    selected[2*binding-1][1]["binding"] == row["binding"] and
                    number(selected[2*binding-1][1], "home_va") == homes[binding-1],
                    "negative execution authorization or geometry mismatch")
        registered, unbound = selected[6][1], selected[-1][1]
        require(registered["op"] == unbound["op"] == operation and
                registered["bindings"] == unbound["bindings"] == "3" and
                registered["requester_cna"] == positive[0]["requester_cna"] and
                unbound["reason"] == "completion_failure" and
                (unbound["load_bytes"], unbound["store_bytes"], unbound["fences"],
                 unbound["segment_payload_staging_bytes"]) ==
                (str(2 * tensor_bytes), "0", "0", "0"),
                "negative execution completion accounting mismatch")
        for binding, (_, row) in enumerate(selected[7:-1], 1):
            authorization = auth[binding-1]
            require(number(row, "binding") == binding and number(row, "length") == tensor_bytes and
                    number(row, "addr") == number(authorization, "local") and
                    all(row[key] == authorization[key] for key in ("map", "generation")),
                    "negative execution input callback mismatch")
    positive_position = next(position for position, line in enumerate(lines)
                             if line.startswith(prefixes[0] + " ") and
                             records(line, prefixes[0])[0]["request"] == positive_request)
    require(ordered_positions == sorted(ordered_positions) and
            ordered_positions[-1] < positive_position,
            "negative execution must drain before recovery compute")
    return "\n".join(line for position, line in enumerate(lines) if position not in consumed)


def validate_readonly_inputs(guests, maps, output_maps, requests, required,
                             geometry=GEOMETRIES["cross-page"]):
    _, object_bytes, offsets = geometry
    probes = [records(text, "MANAGED_PTO_READONLY") for text in guests]
    pairs = [records(text, "MANAGED_OWNER_PAIR") for text in guests]
    if not required:
        require(not any(probes + pairs), "unexpected read-only mapping evidence")
        return
    require(not probes[0] and [row["probe"] for row in probes[1]] ==
            ["sdk-write-denied", "cpu-write-denied", "input-unchanged"],
            "missing or unexpected read-only probes")
    require(all(row["node"] == "1" and row["object"] == "input" for row in probes[1]),
            "read-only probe used the wrong mapping")
    sdk, cpu, unchanged = probes[1]
    require(sdk["cpu_rc"] == sdk["compute_rc"] == "-13" and
            sdk["cpu_pins"] == "1" and sdk["compute_pins"] == sdk["in_flight_pins"] == "0",
            "read-only SDK admitted a write pin or leaked resources")
    require(number(cpu, "address") == number(maps[1], "cpu_base") + offsets[0] and
            number(cpu, "signal") in (7, 11) and number(cpu, "code") > 0 and
            cpu["unchanged"] == "pass" and re.fullmatch(r"[0-9a-f]{16}", cpu["checksum"]),
            "missing original-process CPU write protection")
    require(number(unchanged, "bytes") == object_bytes and unchanged["unchanged"] == "pass" and
            unchanged["checksum"] == cpu["checksum"], "read-only input object changed during PTO")
    lines = guests[1].splitlines()
    probe_positions = [i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_READONLY ")]
    first_submit = next(i for i, line in enumerate(lines) if
        line.startswith("MANAGED_PTO_NEGATIVE ") or
        line.startswith("MANAGED_PTO node=1 dispatch=1 op_id="))
    last_completion = next(i for i, line in enumerate(lines) if
        line.startswith("MANAGED_PTO node=1 dispatch=2 completion_op="))
    ready = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                 "cpu_verify=" in line and records(line, "MANAGED_PTO")[0]["vector"] == "1")
    drained = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ") and
                   "state=drained" in line and records(line, "MANAGED_OWNER")[0]["object"] == "input")
    require(ready < probe_positions[0] < probe_positions[1] < first_submit and
            last_completion < probe_positions[2] < drained,
            "read-only checks do not surround the original compute sequence")
    for node, rows in enumerate(pairs):
        require(len(rows) == 1 and rows[0]["node"] == str(node) and
                rows[0]["shared_domain"] == "1" and
                rows[0]["input_mapping_id"] == maps[node]["mapping_id"] and
                rows[0]["output_mapping_id"] == output_maps[node]["mapping_id"],
                "owners do not share the intended access domain")
    published = [row for row in records(guests[0], "MANAGED_PTO") if "inputs_published" in row]
    require([row.get("object") for row in published] == ["input", "output"] and
            all(row["inputs_published"] == row["version"] == "1" and number(row, "bytes") == object_bytes
                for row in published), "both objects must be initialized and published")


def validate_readonly_owners(guests, maps, outputs, requests, recovery, threads):
    require(0 <= threads <= 8, "invalid access thread count")
    for node, text in enumerate(guests):
        lines = text.splitlines()
        rows = records(text, "MANAGED_OWNER")
        require(all(row.get("object") in ("input", "output") for row in rows),
                "unexpected owner object")
        drained_positions = []
        result = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_RESULT "))
        for label, mapping in (("input", maps[node]), ("output", outputs[node])):
            selected = [row for row in rows if row["object"] == label]
            phases = ["adopted", "drained"] if node == 0 else (
                ["adopted", "closing", "workers-drained", "drained"] if label == "input" else
                ["adopted", "closing", "drained"])
            require([row["state"] for row in selected] == phases and
                    all(row["node"] == str(node) for row in selected), "owner lifecycle sequence mismatch")
            adopted, drained = selected[0], selected[-1]
            require(adopted["generation"] == mapping["generation"] and
                    adopted["mapping_id"] == mapping["mapping_id"] and
                    adopted["cpu_pins"] == adopted["holder_transferred"] == "1",
                    "owner did not adopt the intended object")
            require(all(drained[key] == "0" for key in ("cpu_pins", "compute_pins", "in_flight_pins",
                        "mapping_pending", "holder_pending")), "owner resources not drained")
            positions = [i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ") and
                         records(line, "MANAGED_OWNER")[0]["object"] == label]
            first_access = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                records(line, "MANAGED_PTO")[0].get("object") == label and
                ("inputs_published=" in line or "cpu_verify=" in line))
            require(positions[0] < first_access and positions[-1] < result,
                    "owner lifetime does not bracket CPU access")
            drained_positions.append(positions[-1])
            if node == 0:
                continue
            closing = selected[1]
            count = threads if label == "input" else 0
            compute = (1 if label == "input" else 2) if recovery else 0
            require(number(closing, "threads") == count and number(closing, "cpu_pins") == count + 1 and
                    number(closing, "compute_pins") == number(closing, "in_flight_pins") == compute and
                    closing["close_guard"] == closing["admission_guard"] == "pass" and
                    closing["operation_id"] == (requests[1] if recovery else "0"),
                    "read-only owner close/admission guard mismatch")
            completion = next(i for i, line in enumerate(lines) if
                line.startswith("MANAGED_PTO node=1 dispatch=2 completion_op="))
            if recovery:
                submit = next(i for i, line in enumerate(lines) if
                    line.startswith("MANAGED_PTO node=1 dispatch=2 op_id="))
                timeout = next(i for i, line in enumerate(lines) if
                    line.startswith("MANAGED_PTO_WAIT node=1 dispatch=2 ") and "state=timeout" in line)
                require(submit < positions[1] < timeout < completion,
                        "owner guard outside the original pending operation")
            else:
                require(completion < positions[1], "owner sealed before the last compute completion")
            if label == "input":
                workers = selected[2]
                require(number(workers, "threads") == number(workers, "after_seal_threads") == threads and
                        number(workers, "checks") >= threads and workers["cpu_verify"] == "pass" and
                        completion < positions[2], "CPU workers were not verified and drained")
        require(drained_positions[1] < drained_positions[0], "shared domain owner drained too early")


def validate_owners(guests, maps, requests, recovery, threads):
    require(0 <= threads <= 8, "invalid access thread count")
    for node, text in enumerate(guests):
        rows = records(text, "MANAGED_OWNER")
        states = ["adopted", "drained"] if node == 0 else [
            "adopted", "closing", "workers-drained", "drained"]
        require([r["state"] for r in rows] == states and
                all(number(r, "node") == node for r in rows), "owner lifecycle sequence mismatch")
        adopted, drained = rows[0], rows[-1]
        require(adopted["generation"] == maps[node]["generation"] and
                adopted["mapping_id"] == maps[node]["mapping_id"] and
                adopted["cpu_pins"] == adopted["holder_transferred"] == "1",
                "owner did not adopt the existing mapping and holder")
        require(all(drained[k] == "0" for k in ("cpu_pins", "compute_pins", "in_flight_pins",
                "mapping_pending", "holder_pending")), "owner resources not drained")
        lines = text.splitlines()
        owner_positions = [i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ")]
        result_position = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_RESULT "))
        require(owner_positions[-1] < result_position, "owner drained after success report")
        first_verify = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ")
                            and "cpu_verify=" in line)
        require(owner_positions[0] < first_verify, "CPU accessed before owner adoption")
        if node == 0:
            continue
        closing, workers = rows[1:3]
        compute = 3 if recovery else 0
        require(number(closing, "threads") == threads and
                number(closing, "cpu_pins") == threads + 1 and
                number(closing, "compute_pins") == number(closing, "in_flight_pins") == compute and
                closing["close_guard"] == closing["admission_guard"] == "pass" and
                closing["operation_id"] == (requests[1] if recovery else "0"),
                "owner in-use close/admission guard mismatch")
        require(number(workers, "threads") == number(workers, "after_seal_threads") == threads and
                number(workers, "checks") >= threads and workers["cpu_verify"] == "pass",
                "worker CPU verification or post-seal access missing")
        completion_position = next(i for i, line in enumerate(lines)
                                   if line.startswith("MANAGED_PTO node=1 dispatch=2 completion_op="))
        require(completion_position < owner_positions[2], "workers drained before original completion")
        if recovery:
            submit_position = next(i for i, line in enumerate(lines)
                                   if line.startswith("MANAGED_PTO node=1 dispatch=2 op_id="))
            timeout_position = next(i for i, line in enumerate(lines)
                                    if line.startswith("MANAGED_PTO_WAIT node=1 dispatch=2 ") and
                                    "state=timeout" in line)
            require(submit_position < owner_positions[1] < timeout_position < completion_position,
                    "owner guard outside original pending operation")
        else:
            require(completion_position < owner_positions[1], "CPU-only guard before compute completion")


def validate_callbacks(text, mapping, requests, output_mapping=None,
                       geometry=GEOMETRIES["cross-page"]):
    elements, _, vector_offsets = geometry
    tensor_bytes = elements * 4
    require(not re.search(r"QEMU_UB_GM_.*(?: denied |REJECT |FAILURE_COMPLETION |TIMEOUT |METADATA_CRC_FAIL )", text),
            "QEMU rejected a managed dispatch or callback")
    bound = records(text, "QEMU_UB_GM_GSVA_BIND")
    require(len(bound) == 6, "strict GSVA binding evidence missing")
    validate_backing_identities(bound, [mapping, output_mapping or mapping])
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
    bases = {}
    for index, request in enumerate(requests):
        select = lambda rows: [r for r in rows if r["request"] == request]
        ins, outs, gsva = select(inputs), select(outputs), select(bound)
        require((len(ins), len(outs), len(gsva)) == (2, 1, 3), "incomplete request bindings")
        auth = ins + outs
        require(len({r["binding"] for r in auth}) == 3 and
                {r["binding"] for r in auth} == {r["binding"] for r in gsva} and
                all(number(r, "length") == tensor_bytes for r in auth), "binding identities differ")
        offsets = [vector_offsets[v] for v in ((0, 1, 2) if index == 0 else (2, 1, 3))]
        output = output_mapping or mapping
        homes = [number(row, "cpu_base") for row in
                 ((mapping if index == 0 else output), mapping, output)]
        gsva_by_binding = {row["binding"]: row for row in gsva}
        for row, offset, home in zip(auth, offsets, homes):
            base = number(row, "local") - offset
            require(base > 0 and number(gsva_by_binding[row["binding"]], "home_va") == home and
                    (home not in bases or bases[home] == base), "PTO range geometry mismatch")
            bases[home] = base
        read, write, fence = select(loads), select(stores), select(fences)
        require([r["binding"] for r in read] == [r["binding"] for r in ins] and
                len(write) == len(fence) == 1 and
                write[0]["binding"] == fence[0]["binding"] == outs[0]["binding"] and
                all(number(r, "length") == tensor_bytes for r in read + write + fence),
                "callback permissions, range or binding mismatch")
        authorizations = {row["binding"]: row for row in auth}
        for row in read + write + fence:
            require(number(row, "addr") == number(authorizations[row["binding"]], "local"),
                    "callback address differs from the authorized object range")
        for row in read + write:
            require(all(number(row, field) == number(authorizations[row["binding"]], field)
                        for field in ("map", "generation")), "callback mapping identity mismatch")
        registered, unbound = registers[index], unbinds[index]
        require(registered["op"] == unbound["op"] == request and
                registered["bindings"] == "3" and unbound["bindings"] == "3" and
                unbound["reason"] == "completion_success" and
                (unbound["load_bytes"], unbound["store_bytes"], unbound["fences"],
                 unbound["segment_payload_staging_bytes"]) ==
                (str(2 * tensor_bytes), str(tensor_bytes), "1", "0"),
                "PTO completion accounting or zero-staging evidence mismatch")
    require(len(set(bases.values())) == len(bases), "distinct objects share a physical view")
    require(number(outputs[0], "local") == number(inputs[2], "local"),
            "TSTORE output is not the next TLOAD input")


def validate_transport_chunks(text, requests, geometry):
    """Match existing strict I/O trace pairs to the following completed callback.

    CPU MMIO traffic is at most eight bytes. Legacy bootstrap page reads also
    appear in this log. Count strict bulk I/O only inside a registered PTO
    dispatch; reject out-of-window traffic to any of its known backing routes.
    Bulk PTO I/O holds the QEMU I/O-thread lock; each callback is logged only
    after all chunks complete.
    The existing sender labels both reads and writes 'read send'. Classify
    direction using the matched LOAD/STORE callback, never that label.
    """
    limit = 4055  # 4095-byte payload minus the 40-byte UBCGsvaIoReq header.
    _, _, offsets = geometry
    peers = {(row["request"], row["binding"]): number(row, "peer_cna")
             for role in ("INPUT", "OUTPUT")
             for row in records(text, "QEMU_UB_GM_" + role + "_AUTHORIZE")}
    strict_routes = {(number(row, "backing_token"), peers[(row["request"], row["binding"])])
                     for row in records(text, "QEMU_UB_GM_GSVA_BIND")}
    auth, backing, observed, seen = {}, {}, [], set()
    pending, active = None, None
    non_pto_bulk_sends = 0
    counts = {"load": 0, "store": 0, "load_bytes": 0, "store_bytes": 0, "callbacks": 0}
    for line in text.splitlines():
        io_line = line.startswith("ubc sim_dec ")
        if not io_line and not line.startswith(("QEMU_UB_GM_", "SIM_QEMU_UB_GM_BIND_REGISTER ")):
            continue
        rows = [] if io_line else records(line, "SIM_QEMU_UB_GM_BIND_REGISTER")
        if rows:
            require(active is None and pending is None and not observed,
                    "overlapping PTO transport windows")
            active = rows[0]["request"]
        rows = [] if io_line else records(line, "QEMU_UB_GM_UNBIND")
        if rows:
            require(active == rows[0]["request"] and pending is None and not observed,
                    "PTO transport window closed with an incomplete transfer")
            active = None
        for prefix in ("QEMU_UB_GM_INPUT_AUTHORIZE", "QEMU_UB_GM_OUTPUT_AUTHORIZE"):
            rows = [] if io_line else records(line, prefix)
            if rows:
                row = rows[0]
                auth[(row["request"], row["binding"])] = row
        rows = [] if io_line else records(line, "QEMU_UB_GM_GSVA_BIND")
        if rows:
            row = rows[0]
            backing[(row["request"], row["binding"])] = row
        rows = records(line, "ubc sim_dec read send")
        if rows and number(rows[0], "len") <= 8:
            continue
        if rows and number(rows[0], "len") > 8:
            row = rows[0]
            if active is None:
                require((number(row, "token"), number(row, "dcna")) not in strict_routes
                        and row["req"] not in seen,
                        "strict bulk transfer outside a registered PTO dispatch")
                non_pto_bulk_sends += 1
                continue
            require(pending is None and 8 < number(row, "len") <= limit and
                    number(row, "done") == sum(number(r, "len") for r in observed),
                    "transport chunk size, ordering or pending response mismatch")
            require(number(row, "req") > 0 and row["req"] not in seen and
                    number(row, "dcna") > 0 and number(row, "local_cna") > 0 and
                    number(row, "dcna") != number(row, "local_cna"),
                    "transport chunk is repeated or not remote")
            seen.add(row["req"])
            pending = row
        rows = records(line, "ubc sim_dec rx read_resp")
        if rows:
            row = rows[0]
            if active is None:
                require(row["req"] not in seen, "replayed PTO transport response")
                continue
            if pending is not None and row["req"] == pending["req"]:
                require(number(row, "status") == 0 and
                        all(number(row, key) == number(pending, "len") for key in
                            ("data_len", "wire_len", "copy_len", "expect")) and
                        number(row, "peer") == number(pending, "dcna"),
                        "transport response failed or returned an inexact range")
                observed.append(pending)
                pending = None
            else:
                require(number(row, "data_len") <= 8, "unmatched bulk transport response")
        if io_line:
            continue
        for kind in ("LOAD", "STORE"):
            rows = records(line, "QEMU_UB_GM_" + kind)
            if not rows:
                continue
            row = rows[0]
            key = row["request"], row["binding"]
            require(active == row["request"] and key in auth and key in backing and
                    pending is None and len(observed) > 1,
                    "multi-chunk callback has no complete transport evidence")
            length = number(row, "length")
            expected = [min(limit, length - done) for done in range(0, length, limit)]
            require([number(r, "len") for r in observed] == expected,
                    "callback transport chunks have gaps, duplication or truncation")
            views = (2, 1, 3) if row["request"] == requests[1] else (0, 1, 2)
            offset = offsets[views[number(row, "binding") - 1]]
            identity = backing[key]
            for chunk in observed:
                require(number(chunk, "uba") == number(identity, "home_va") + offset +
                        number(chunk, "done") and
                        number(chunk, "token") == number(identity, "backing_token") and
                        number(chunk, "dcna") == number(auth[key], "peer_cna"),
                        "transport chunk targets the wrong backing, offset, token or peer")
            counts[kind.lower()] += len(observed)
            counts[kind.lower() + "_bytes"] += length
            counts["callbacks"] += 1
            observed = []
    require(active is None and pending is None and not observed and counts["callbacks"] >= 6,
            "unfinished or missing transport chunk sequence")
    return {**counts, "chunk_limit_bytes": limit, "request_response_verified": True,
            "scope": "registered-pto-dispatches", "non_pto_bulk_sends": non_pto_bulk_sends}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", required=True, type=Path)
    parser.add_argument("--node-b", required=True, type=Path)
    parser.add_argument("--qemu-b", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-timeout-recovery", action="store_true")
    parser.add_argument("--access-threads", type=int, choices=range(9))
    parser.add_argument("--require-negative-checks", action="store_true")
    parser.add_argument("--require-readonly-inputs", action="store_true")
    parser.add_argument("--layout", choices=GEOMETRIES, default="cross-page")
    args = parser.parse_args()
    paths = [args.node_a, args.node_b, args.qemu_b]
    try:
        contents = [path.read_bytes() for path in paths]
        text = [data.decode("utf-8", errors="replace") for data in contents]
        result = validate(text[:2], text[2], args.require_timeout_recovery,
                          args.access_threads, args.require_negative_checks,
                          args.require_readonly_inputs, args.layout)
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
