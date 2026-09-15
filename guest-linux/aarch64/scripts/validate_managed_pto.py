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
from validate_mem_service_reference_guest import decode_reference


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
    # QEMU logs the allocation's exported backing base. A V2 CPU view can
    # start inside that allocation; its exact offset is checked separately.
    expected = {number(mapping, "address") for mapping in mappings}
    identities = {}
    for row in bound:
        identity = tuple(number(row, key) for key in fields)
        home = number(row, "home_va")
        require(all(identity) and home in expected, "PTO backing does not match a CPU object")
        require(home not in identities or identities[home] == identity,
                "PTO backing identity changed")
        identities[home] = identity
    require(set(identities) == expected, "missing object backing identity")


def validate_home_exports(text, mappings, qemu):
    # Worker startup/work notices contain prose; only export events carry this contract.
    export_events = "\n".join(line for line in text.splitlines()
                              if "phase=exported" in line.split())
    exports = records(export_events, "obmm-worker")
    bindings = records(qemu, "QEMU_UB_GM_GSVA_BIND")
    for mapping in mappings:
        owned = [row for row in exports if row["generation"] == mapping["generation"]]
        require(len(owned) == 1 and number(owned[0], "segment") > 0 and
                number(owned[0], "export") > 0, "selected home has no unique exported allocation")
        backed = [row for row in bindings if number(row, "home_va") == number(mapping, "address")]
        require(backed and all(number(row, "segment") == number(owned[0], "segment") for row in backed),
                "compute binding does not reference the selected home export")


def validate(guests, qemu, require_timeout_recovery=False, access_threads=None,
             require_negative_checks=False, require_readonly_inputs=False, layout="cross-page",
             home_node=None, concurrent_requests=1, require_reference_inputs=False,
             strict_io_version=1):
    require(strict_io_version in (1, 2), "unsupported strict I/O wire version")
    require(not require_reference_inputs or require_readonly_inputs,
            "V2 inputs require separate read-only input objects")
    require(concurrent_requests in (1, 2), "unsupported concurrent request count")
    require(concurrent_requests == 1 or not (require_negative_checks or require_timeout_recovery),
            "queued requests cannot use negative or short-wait recovery mode")
    require(home_node in (None, 0, 1), "unsupported home node")
    home = 0 if home_node is None else home_node
    consumer = 1 - home
    require(len(guests) == 2, "expected two actual guest logs")
    require(layout in GEOMETRIES, "unsupported managed geometry")
    elements, object_bytes, offsets = GEOMETRIES[layout]
    tensor_bytes = elements * 4
    reference_offset = offsets[0] if require_reference_inputs else 0
    reference_bytes = offsets[1] + tensor_bytes - offsets[0] if require_reference_inputs else object_bytes
    offsets = tuple(offset - reference_offset if index < 2 else offset
                    for index, offset in enumerate(offsets))
    # Serial logs use CRLF; normalize only parsing, never the evidence bytes.
    guests = [text.replace("\r\n", "\n") for text in guests]
    guests = [guests[home], guests[consumer]]
    maps, output_maps, dispatches = [], [], []
    for role, text in enumerate(guests):
        node = (home, consumer)[role]
        for prefix in ("MANAGED_PTO", "MANAGED_PTO_RESULT", "MANAGED_PTO_CONFIG",
                       "MANAGED_PTO_WAIT", "MANAGED_PTO_NEGATIVE", "MANAGED_PTO_NEGATIVE_DATA",
                       "MANAGED_PTO_READONLY", "MANAGED_OWNER", "MANAGED_OWNER_PAIR",
                       "MANAGED_PTO_QUEUE", "MANAGED_PTO_BATCH", "MANAGED_PTO_REFERENCE"):
            require(all(number(row, "node") == node for row in records(text, prefix)),
                    "actual node identity differs from its guest log")
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
            view_offset = reference_offset if index == 0 else 0
            view_bytes = reference_bytes if index == 0 else object_bytes
            require(number(record, "node") == node and
                    number(record, "address") > 0 and
                    number(record, "cpu_base") == number(record, "address") + view_offset and
                    number(record, "logical_bytes") == view_bytes and
                    number(record, "generation") > 0 and number(record, "mapping_id") > 0,
                    f"node {node}: invalid managed identity")
            if home_node is not None:
                require(number(record, "home_node") == home and
                        record["home_provider"] == f"guest-node-{home}",
                        "managed mapping has the wrong home provider")
            expected_access = 1 if require_readonly_inputs and index == 0 and (
                role == 1 or require_reference_inputs) else 3
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
        require(int(done[0].get("concurrent_requests", "1")) == concurrent_requests,
                "guest result used a different compute mode")
        require(done[0].get("reference_inputs", "0") == str(int(require_reference_inputs)),
                "missing or unexpected V2 input mode")
        verified = [r for r in rows if "cpu_verify" in r]
        expected_vectors = [2, 3] if role == 0 else [0, 1, 2, 3]
        require([number(r, "vector") for r in verified] == expected_vectors,
                f"node {node}: missing or repeated CPU checks")
        for row, vector in zip(verified, expected_vectors):
            require(row["cpu_verify"] == "pass" and number(row, "bytes") == tensor_bytes and
                    row["checksum"] == oracle_checksum(
                        2 if vector == 3 and concurrent_requests == 2 else vector, elements) and
                    number(row, "version") == (1 if vector < 2 else vector),
                    f"node {node}: CPU oracle mismatch")
            if vector == 3 and concurrent_requests == 2:
                require(row["path"] == ("remote-" if role == 0 else "") + "tstore-cpu-independent",
                        "independent output was reported as a dependent computation")
            if require_readonly_inputs:
                require(row.get("object") == ("input" if vector < 2 else "output"),
                        "CPU verified the wrong object")
        for metric in ("put_object_count", "materialize_object_count"):
            require(re.findall(rf"^{metric}=([0-9]+)$", text, re.M) == ["0"],
                    f"node {node}: unexpected control-plane data operation")
        validate_kernel_regions(text)
        if role == 1:
            dispatches = [r for r in rows if "retained_leases" in r]
            completions = [r for r in rows if "completion_op" in r]
            require([number(r, "dispatch") for r in dispatches] == [1, 2] and
                    (sorted(number(r, "dispatch") for r in completions) if concurrent_requests == 2 else
                     [number(r, "dispatch") for r in completions]) == [1, 2],
                    "expected two distinct dispatches and completions")
            completions = sorted(completions, key=lambda row: number(row, "dispatch"))
            for request, completion, views in zip(dispatches, completions,
                    ((0, 1, 2), (0 if concurrent_requests == 2 else 2, 1, 3))):
                require(request["op_id"] == completion["completion_op"] and
                        request["generation"] == mapping["generation"] and
                        request["mapping_id"] == mapping["mapping_id"] and
                        request["offsets"] == ",".join(str(offsets[v]) for v in views) and
                        number(request, "tensor_bytes") == tensor_bytes and
                        request["retained_leases"] == "3" and request["release_guard"] == "pass" and
                        completion["status"] == "1" and completion["error"] == "none",
                        "dispatch identity, geometry, lifetime or completion mismatch")
                if require_readonly_inputs:
                    first = mapping if request["dispatch"] == "1" or concurrent_requests == 2 else output
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
        require(number(row, "node") == consumer and number(row, "dispatch") == index // 2 + 1 and
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
    if concurrent_requests == 2:
        require(events[:2] == [("submit", str(i + 1), request) for i, request in enumerate(requests)] and
                sorted(events[2:]) == [("completion", str(i + 1), request)
                                       for i, request in enumerate(requests)],
                "queued submissions did not precede both original completions")
    else:
        require(events == [(phase, str(index + 1), request)
                           for index, request in enumerate(requests) for phase in phases],
                "PTO submission, wait and completion order mismatch")
    validate_batch(guests, requests, concurrent_requests)
    require(not (require_negative_checks and require_timeout_recovery),
            "negative checks cannot use short-wait recovery")
    output = output_maps[1] if require_readonly_inputs else None
    geometry = (elements, object_bytes, offsets)
    filtered = validate_negatives(guests, qemu, maps[1], requests, require_negative_checks, output, geometry)
    validate_callbacks(filtered, maps[0], requests, output, geometry, concurrent_requests)
    if home_node is not None:
        validate_home_exports(guests[0], [maps[0], output_maps[0]] if require_readonly_inputs else
                              [maps[0]], qemu)
        requester = number(maps[1], "requester_cna")
        require(requester > 0 and number(output_maps[1], "requester_cna") == requester,
                "inconsistent compute-node requester identity")
        for prefix in ("SIM_QEMU_UB_GM_BIND_REGISTER", "QEMU_UB_GM_INPUT_AUTHORIZE",
                       "QEMU_UB_GM_OUTPUT_AUTHORIZE"):
            require(all(number(row, "requester_cna") == requester for row in records(qemu, prefix)),
                    "QEMU evidence belongs to another compute requester")
    validate_readonly_inputs(guests, maps, output_maps, requests, require_readonly_inputs, geometry,
                             reference_bytes)
    reference = validate_reference_inputs(guests, maps, require_reference_inputs, GEOMETRIES[layout])
    transport = validate_transport_chunks(qemu, requests, geometry, concurrent_requests,
                                          strict_io_version) if layout != "cross-page" else None
    if access_threads is not None:
        if require_readonly_inputs:
            validate_readonly_owners(guests, maps, output_maps, requests,
                                     require_timeout_recovery, access_threads, concurrent_requests,
                                     require_reference_inputs)
        else:
            validate_owners(guests, maps, requests, require_timeout_recovery, access_threads,
                            concurrent_requests)
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
            "reference_inputs_verified": require_reference_inputs, "reference": reference,
            "owner_verified": access_threads is not None, "access_threads": access_threads,
            "timeout_recoveries": len(waits) // 2,
            "layout": layout, "elements": elements, "tensor_bytes": tensor_bytes,
            "object_bytes": object_bytes, "transport_chunks": transport,
            "am2_complete": False,
            "concurrent_requests": concurrent_requests,
            "queued_overlap_verified": concurrent_requests == 2,
            "device_parallel_execution_verified": False,
            **({"home_node": home, "compute_node": consumer, "home_export_verified": True}
               if home_node is not None else {})}


def validate_batch(guests, requests, count):
    prefixes = ("MANAGED_PTO_QUEUE", "MANAGED_PTO_BATCH")
    require(not any(records(guests[0], prefix) for prefix in prefixes),
            "home unexpectedly submitted a batch")
    events = [(i, prefix, records(line, prefix)[0])
              for i, line in enumerate(guests[1].splitlines())
              for prefix in prefixes if line.startswith(prefix + " ")]
    if count == 1:
        require(not events, "unexpected queued computation evidence")
        return
    require(len(events) == 6 and [row["state"] for _, _, row in events] ==
            ["queued", "queued", "overlap", "completed", "completed", "drained"],
            "missing or reordered batch lifetime events")
    require([prefix for _, prefix, _ in events] ==
            [prefixes[0], prefixes[0], prefixes[1], prefixes[0], prefixes[0], prefixes[1]],
            "incorrect queue/batch event type")
    queued = [events[0][2], events[1][2]]
    overlap, drained = events[2][2], events[5][2]
    completed = [events[3][2], events[4][2]]
    for index, row in enumerate(queued):
        require(row["op_id"] == requests[index] and number(row, "dispatch") == index + 1 and
                number(row, "queued_requests") == index + 1 and row["retained_leases"] == "3",
                "queued request identity, ownership or capacity mismatch")
    addresses = [number(row, "metadata_iova") for row in queued]
    require(all(address > 0 and address % 4096 == 0 for address in addresses) and
            addresses[0] != addresses[1], "concurrent requests reused metadata storage")
    require(overlap["callers"] == overlap["queued_requests"] == "2" and
            overlap["retained_leases"] == "6" and overlap["metadata_distinct"] == "1" and
            overlap["close_guard"] == overlap["release_guard"] == "pass" and
            overlap["op_ids"] == ",".join(requests), "batch did not retain both original operations")
    require([row["queued_requests"] for row in completed] == ["1", "0"] and
            sorted((number(row, "dispatch"), row["op_id"]) for row in completed) ==
            list(enumerate(requests, 1)), "queued completions were duplicated, lost or misattributed")
    require(drained["joined_callers"] == "2" and
            drained["queued_requests"] == drained["compute_leases"] == "0",
            "batch callers or leases were not drained")
    lines = guests[1].splitlines()
    for index, op_id in enumerate(requests):
        submit = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                      f"op_id={op_id}" in line.split())
        completion = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                          f"completion_op={op_id}" in line.split())
        collected = next(i for i, _, row in events if row["state"] == "completed" and row["op_id"] == op_id)
        require(submit < events[index][0] < events[2][0] < collected < completion < events[5][0],
                "batch released a request before its original completion")
    closing = [i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ") and
               "state=closing" in line.split()]
    require(closing and all(events[1][0] < i < events[2][0] for i in closing),
            "owner guards did not protect both queued requests")
    cpu_outputs = [i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                   "cpu_verify=pass" in line.split() and
                   records(line, "MANAGED_PTO")[0]["vector"] in ("2", "3")]
    workers = [i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ") and
               "state=workers-drained" in line.split()]
    require(len(cpu_outputs) == 2 and len(workers) == 1 and
            events[5][0] < min(cpu_outputs) <= max(cpu_outputs) < workers[0],
            "batch outputs or CPU workers were released before both callers joined")


def validate_negatives(guests, qemu, mapping, requests, required, output_mapping=None,
                       geometry=GEOMETRIES["cross-page"]):
    consumer = mapping["node"]
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
                all(row["case"] == name and row["op_id"] == operation and row["node"] == consumer
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
            require(row["node"] == consumer and row["case"] == name and row["op_id"] == operation and
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
                          if line.startswith(f"MANAGED_PTO node={consumer} dispatch=1 op_id="))
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
    homes = [number(row, "address") for row in (mapping, mapping, output_mapping or mapping)]
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
                             geometry=GEOMETRIES["cross-page"], input_bytes=None):
    consumer = maps[1]["node"]
    _, object_bytes, offsets = geometry
    probes = [records(text, "MANAGED_PTO_READONLY") for text in guests]
    pairs = [records(text, "MANAGED_OWNER_PAIR") for text in guests]
    if not required:
        require(not any(probes + pairs), "unexpected read-only mapping evidence")
        return
    require(not probes[0] and [row["probe"] for row in probes[1]] ==
            ["sdk-write-denied", "cpu-write-denied", "input-unchanged"],
            "missing or unexpected read-only probes")
    require(all(row["node"] == consumer and row["object"] == "input" for row in probes[1]),
            "read-only probe used the wrong mapping")
    sdk, cpu, unchanged = probes[1]
    require(sdk["cpu_rc"] == sdk["compute_rc"] == "-13" and
            sdk["cpu_pins"] == "1" and sdk["compute_pins"] == sdk["in_flight_pins"] == "0",
            "read-only SDK admitted a write pin or leaked resources")
    require(number(cpu, "address") == number(maps[1], "cpu_base") + offsets[0] and
            number(cpu, "signal") in (7, 11) and number(cpu, "code") > 0 and
            cpu["unchanged"] == "pass" and re.fullmatch(r"[0-9a-f]{16}", cpu["checksum"]),
            "missing original-process CPU write protection")
    require(number(unchanged, "bytes") == (object_bytes if input_bytes is None else input_bytes) and
            unchanged["unchanged"] == "pass" and
            unchanged["checksum"] == cpu["checksum"], "read-only input object changed during PTO")
    lines = guests[1].splitlines()
    probe_positions = [i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_READONLY ")]
    first_submit = next(i for i, line in enumerate(lines) if
        line.startswith("MANAGED_PTO_NEGATIVE ") or
        line.startswith(f"MANAGED_PTO node={consumer} dispatch=1 op_id="))
    last_completion = next(i for i, line in enumerate(lines) if
        line.startswith(f"MANAGED_PTO node={consumer} dispatch=2 completion_op="))
    ready = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                 "cpu_verify=" in line and records(line, "MANAGED_PTO")[0]["vector"] == "1")
    drained = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ") and
                   "state=drained" in line and records(line, "MANAGED_OWNER")[0]["object"] == "input")
    require(ready < probe_positions[0] < probe_positions[1] < first_submit and
            last_completion < probe_positions[2] < drained,
            "read-only checks do not surround the original compute sequence")
    for node, rows in enumerate(pairs):
        require(len(rows) == 1 and rows[0]["node"] == maps[node]["node"] and
                rows[0]["shared_domain"] == "1" and
                rows[0]["input_mapping_id"] == maps[node]["mapping_id"] and
                rows[0]["output_mapping_id"] == output_maps[node]["mapping_id"],
                "owners do not share the intended access domain")
    published = [row for row in records(guests[0], "MANAGED_PTO") if "inputs_published" in row]
    require([row.get("object") for row in published] == ["input", "output"] and
            all(row["inputs_published"] == row["version"] == "1" and number(row, "bytes") == object_bytes
                for row in published), "both objects must be initialized and published")


def validate_reference_inputs(guests, maps, required, geometry):
    traces = [records(text, "MANAGED_PTO_REFERENCE") for text in guests]
    if not required:
        require(not any(traces), "unexpected V2 reference evidence")
        return None
    elements, allocation_bytes, offsets = geometry
    expected_actions = (("1", "2", "3", "4", "mapped"), ("4", "5", "mapped"))
    references = []
    for role, (text, rows, mapping, actions) in enumerate(zip(guests, traces, maps, expected_actions)):
        require(tuple(row["action"] for row in rows) == actions,
                "V2 publication or acquisition sequence incomplete")
        require(all(row["node"] == mapping["node"] for row in rows), "wrong V2 node")
        encoded = rows[-1]["reference"]
        ref = decode_reference(encoded)
        require(ref["magic"] == 0x514f424d4d524546 and ref["layout"] == 2 and
                ref["kind"] == 1 and ref["state"] == 2 and ref["flags"] == 0 and
                ref["wire_bytes"] == 256 and ref["access"] == 1 and ref["version"] > 1 and
                ref["incarnation"] > 0, "invalid sealed read-only V2 contract")
        require(ref["key"] == mapping["key"] and ref["generation"] == number(mapping, "generation") and
                ref["home"] == f"guest-node-{maps[0]['node']}" and
                ref["owner"] == ref["producer"] == number(maps[0], "node") and
                ref["size"] == allocation_bytes and ref["offset"] == offsets[0] and
                ref["length"] == offsets[1] + elements * 4 - offsets[0],
                "V2 allocation identity or view range differs")
        require(number(mapping, "cpu_base") == number(mapping, "address") + ref["offset"] and
                number(mapping, "logical_bytes") == ref["length"], "V2 mapping range differs")
        require(all(row["status"] == "ok" and number(row, "generation") == ref["generation"] and
                    number(row, "version") == ref["version"] for row in rows[:-1]),
                "V2 transition version or generation differs")
        payload = bytearray(ref["length"])
        for vector in (0, 1):
            values = [(i % 8 + 1 if vector == 0 else i % 4 + 1) if i < 64 else
                      (i // 8 + 1 if vector == 0 else i % 8 + 1) for i in range(elements)]
            start = offsets[vector] - offsets[0]
            payload[start:start + elements * 4] = struct.pack(f"<{elements}f", *values)
        require(ref["checksum"] == int(checksum64(payload), 16), "V2 payload checksum differs")
        if role == 1:
            checks = [row for row in records(text, "MANAGED_PTO_READONLY") if "checksum" in row]
            require(len(checks) == 2 and all(int(row["checksum"], 16) == ref["checksum"] for row in checks),
                    "CPU mapping checksum differs from published V2 payload")
        mapped_position = next(i for i, line in enumerate(text.splitlines()) if
                               line.startswith("MANAGED_PTO_REFERENCE ") and "action=mapped " in line)
        owner_position = next(i for i, line in enumerate(text.splitlines()) if
                              line.startswith("MANAGED_OWNER ") and "state=adopted " in line)
        require(mapped_position < owner_position, "owner adopted before V2 mapping")
        if role == 0:
            lines = text.splitlines()
            events = [i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_REFERENCE ")]
            writes = [i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO ") and
                      "inputs_published=" in line and "object=input" in line]
            require(len(writes) == 1 and events[0] < writes[0] < events[1] < events[2] <
                    events[3] < events[4] < owner_position,
                    "V2 writer publication must precede seal and read-only owner adoption")
        references.append(encoded)
    require(references[0] == references[1], "writer and consumer used different complete V2 references")
    return {"layout": 2, "offset": offsets[0], "bytes": len(payload),
            "generation": ref["generation"], "version": ref["version"],
            "incarnation": ref["incarnation"], "checksum": f"{ref['checksum']:016x}"}


def validate_readonly_owners(guests, maps, outputs, requests, recovery, threads, concurrent_requests=1,
                             reference_inputs=False):
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
                    all(row["node"] == mapping["node"] for row in selected), "owner lifecycle sequence mismatch")
            adopted, drained = selected[0], selected[-1]
            require(adopted["generation"] == mapping["generation"] and
                    adopted["mapping_id"] == mapping["mapping_id"] and
                    adopted["cpu_pins"] == adopted["holder_transferred"] == "1",
                    "owner did not adopt the intended object")
            require(all(drained[key] == "0" for key in ("cpu_pins", "compute_pins", "in_flight_pins",
                        "mapping_pending", "holder_pending")), "owner resources not drained")
            positions = [i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ") and
                         records(line, "MANAGED_OWNER")[0]["object"] == label]
            if reference_inputs and node == 0 and label == "input":
                # The writer mapping has already been unmapped and sealed;
                # this owner holds the subsequent read-only reference mapping.
                # validate_reference_inputs checks the complete writer order.
                require(positions[0] < positions[-1] < result, "V2 home owner lifetime is incomplete")
            else:
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
            if concurrent_requests == 2:
                compute = 4 if label == "input" else 2
            require(number(closing, "threads") == count and number(closing, "cpu_pins") == count + 1 and
                    number(closing, "compute_pins") == number(closing, "in_flight_pins") == compute and
                    closing["close_guard"] == closing["admission_guard"] == "pass" and
                    closing["operation_id"] == (requests[1] if recovery or concurrent_requests == 2 else "0"),
                    "read-only owner close/admission guard mismatch")
            completion = next(i for i, line in enumerate(lines) if
                line.startswith(f"MANAGED_PTO node={mapping['node']} dispatch=2 completion_op="))
            if concurrent_requests == 2:
                require(positions[1] < completion, "queued owner guard occurred after completion")
            elif recovery:
                submit = next(i for i, line in enumerate(lines) if
                    line.startswith(f"MANAGED_PTO node={mapping['node']} dispatch=2 op_id="))
                timeout = next(i for i, line in enumerate(lines) if
                    line.startswith(f"MANAGED_PTO_WAIT node={mapping['node']} dispatch=2 ") and "state=timeout" in line)
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


def validate_owners(guests, maps, requests, recovery, threads, concurrent_requests=1):
    require(0 <= threads <= 8, "invalid access thread count")
    for node, text in enumerate(guests):
        rows = records(text, "MANAGED_OWNER")
        states = ["adopted", "drained"] if node == 0 else [
            "adopted", "closing", "workers-drained", "drained"]
        require([r["state"] for r in rows] == states and
                all(r["node"] == maps[node]["node"] for r in rows), "owner lifecycle sequence mismatch")
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
        compute = 6 if concurrent_requests == 2 else 3 if recovery else 0
        require(number(closing, "threads") == threads and
                number(closing, "cpu_pins") == threads + 1 and
                number(closing, "compute_pins") == number(closing, "in_flight_pins") == compute and
                closing["close_guard"] == closing["admission_guard"] == "pass" and
                closing["operation_id"] == (requests[1] if recovery or concurrent_requests == 2 else "0"),
                "owner in-use close/admission guard mismatch")
        require(number(workers, "threads") == number(workers, "after_seal_threads") == threads and
                number(workers, "checks") >= threads and workers["cpu_verify"] == "pass",
                "worker CPU verification or post-seal access missing")
        completion_position = next(i for i, line in enumerate(lines)
                                   if line.startswith(f"MANAGED_PTO node={maps[node]['node']} dispatch=2 completion_op="))
        require(completion_position < owner_positions[2], "workers drained before original completion")
        if concurrent_requests == 2:
            require(owner_positions[1] < completion_position, "queued owner guard occurred after completion")
        elif recovery:
            submit_position = next(i for i, line in enumerate(lines)
                                   if line.startswith(f"MANAGED_PTO node={maps[node]['node']} dispatch=2 op_id="))
            timeout_position = next(i for i, line in enumerate(lines)
                                    if line.startswith(f"MANAGED_PTO_WAIT node={maps[node]['node']} dispatch=2 ") and
                                    "state=timeout" in line)
            require(submit_position < owner_positions[1] < timeout_position < completion_position,
                    "owner guard outside original pending operation")
        else:
            require(completion_position < owner_positions[1], "CPU-only guard before compute completion")


def validate_callbacks(text, mapping, requests, output_mapping=None,
                       geometry=GEOMETRIES["cross-page"], concurrent_requests=1):
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
            (sorted(r["request"] for r in unbinds) == sorted(requests) if concurrent_requests == 2 else
             [r["request"] for r in unbinds] == requests), "PTO completion sequence differs")
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
        offsets = [vector_offsets[v] for v in ((0, 1, 2) if index == 0 else
                                             (0 if concurrent_requests == 2 else 2, 1, 3))]
        output = output_mapping or mapping
        homes = [number(row, "address") for row in
                 ((mapping if index == 0 or concurrent_requests == 2 else output), mapping, output)]
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
        registered, unbound = select(registers)[0], select(unbinds)[0]
        require(registered["op"] == unbound["op"] == request and
                registered["bindings"] == "3" and unbound["bindings"] == "3" and
                unbound["reason"] == "completion_success" and
                (unbound["load_bytes"], unbound["store_bytes"], unbound["fences"],
                 unbound["segment_payload_staging_bytes"]) ==
                (str(2 * tensor_bytes), str(tensor_bytes), "1", "0"),
                "PTO completion accounting or zero-staging evidence mismatch")
    require(len(set(bases.values())) == len(bases), "distinct objects share a physical view")
    if concurrent_requests == 2:
        writes = [number(next(row for row in outputs if row["request"] == request), "local")
                  for request in requests]
        require(abs(writes[0] - writes[1]) >= tensor_bytes, "concurrent output ranges overlap")
    else:
        require(number(outputs[0], "local") == number(inputs[2], "local"),
                "TSTORE output is not the next TLOAD input")


def validate_transport_chunks(text, requests, geometry, concurrent_requests=1,
                              strict_io_version=1):
    """Match existing strict I/O trace pairs to the following completed callback.

    CPU MMIO traffic is at most eight bytes. Legacy bootstrap page reads also
    appear in this log. Count strict bulk I/O only inside a registered PTO
    dispatch; reject out-of-window traffic to any of its known backing routes.
    Bulk PTO I/O holds the QEMU I/O-thread lock; each callback is logged only
    after all chunks complete.
    The existing sender labels both reads and writes 'read send'. Classify
    direction using the matched LOAD/STORE callback, never that label.
    """
    require(strict_io_version in (1, 2), "unsupported strict I/O wire version")
    # Explicit build profile; never infer a version from the observed chunk size.
    limit = 4095 - {1: 40, 2: 128}[strict_io_version]
    _, _, offsets = geometry
    peers = {(row["request"], row["binding"]): number(row, "peer_cna")
             for role in ("INPUT", "OUTPUT")
             for row in records(text, "QEMU_UB_GM_" + role + "_AUTHORIZE")}
    strict_routes = {(number(row, "backing_token"), peers[(row["request"], row["binding"])])
                     for row in records(text, "QEMU_UB_GM_GSVA_BIND")}
    auth, backing, observed, seen = {}, {}, [], set()
    pending, active = None, set()
    non_pto_bulk_sends = 0
    counts = {"load": 0, "store": 0, "load_bytes": 0, "store_bytes": 0, "callbacks": 0}
    for line in text.splitlines():
        io_line = line.startswith("ubc sim_dec ")
        if not io_line and not line.startswith(("QEMU_UB_GM_", "SIM_QEMU_UB_GM_BIND_REGISTER ")):
            continue
        rows = [] if io_line else records(line, "SIM_QEMU_UB_GM_BIND_REGISTER")
        if rows:
            require(rows[0]["request"] not in active and len(active) < concurrent_requests and
                    pending is None and not observed,
                    "overlapping PTO transport windows")
            active.add(rows[0]["request"])
        rows = [] if io_line else records(line, "QEMU_UB_GM_UNBIND")
        if rows:
            require(rows[0]["request"] in active and pending is None and not observed,
                    "PTO transport window closed with an incomplete transfer")
            active.remove(rows[0]["request"])
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
            if not active:
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
            if not active:
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
            require(row["request"] in active and key in auth and key in backing and
                    pending is None and len(observed) > 1,
                    "multi-chunk callback has no complete transport evidence")
            length = number(row, "length")
            expected = [min(limit, length - done) for done in range(0, length, limit)]
            require([number(r, "len") for r in observed] == expected,
                    "callback transport chunks have gaps, duplication or truncation")
            views = (0 if concurrent_requests == 2 else 2, 1, 3) if row["request"] == requests[1] else (0, 1, 2)
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
    require(not active and pending is None and not observed and counts["callbacks"] >= 6,
            "unfinished or missing transport chunk sequence")
    return {**counts, "chunk_limit_bytes": limit, "strict_io_version": strict_io_version,
            "request_response_verified": True,
            "scope": "registered-pto-dispatches", "non_pto_bulk_sends": non_pto_bulk_sends}


def validate_model_probe(guests, qemu, output_bytes, output_checksum):
    """The fixed two-node Qwen layer probe; not a complete W5/PP acceptance."""
    require(len(guests) == 2 and 0 < output_bytes <= 245760 and
            0 <= output_checksum < 1 << 64, "invalid model probe oracle")
    guests = [text.replace("\r\n", "\n") for text in guests]
    maps, references = [], []
    hidden = b"".join(struct.pack("<e", ((i % 31) - 15) / 32) for i in range(1024))
    for node, text in enumerate(guests):
        require("status=fail" not in text and "resources_retained=1" not in text,
                "model guest reported failure")
        for prefix in ("MANAGED_PTO", "MANAGED_PTO_RESULT", "MANAGED_PTO_CONFIG",
                       "MANAGED_MODEL", "MANAGED_OWNER", "MANAGED_PTO_REFERENCE"):
            require(all(number(row, "node") == node for row in records(text, prefix)),
                    "model evidence has wrong guest identity")
        config = records(text, "MANAGED_PTO_CONFIG")
        require(len(config) == 1 and config[0].get("model_probe") == "1" and
                config[0]["callable"] == "4" and config[0]["home_node"] == "1",
                "model probe configuration missing")
        mappings = [r for r in records(text, "MANAGED_PTO") if "cpu_base" in r]
        require(len(mappings) == 2 and [r["object"] for r in mappings] == ["input", "output"],
                "model objects missing")
        for row, length, rights in zip(mappings, (2048, 262144), (1, 3)):
            require(number(row, "cpu_base") == number(row, "address") > 0 and
                    number(row, "logical_bytes") == length and number(row, "access") == rights and
                    number(row, "generation") > 0 and number(row, "mapping_id") > 0 and
                    row["home_node"] == "1" and row["home_provider"] == "guest-node-1",
                    "model mapping identity, range or rights mismatch")
        require(mappings[1]["key"] == mappings[0]["key"] + "-output" and
                mappings[0]["address"] != mappings[1]["address"], "model output aliases input")
        maps.append(mappings)
        refs = records(text, "MANAGED_PTO_REFERENCE")
        require([r["action"] for r in refs] ==
                (["4", "5", "mapped"] if node == 0 else ["1", "2", "3", "4", "mapped"]),
                "model reference lifecycle missing or reordered")
        ref = decode_reference(refs[-1]["reference"])
        require((ref["magic"], ref["layout"], ref["kind"], ref["state"], ref["flags"],
                 ref["wire_bytes"], ref["access"], ref["offset"], ref["length"], ref["size"]) ==
                (0x514f424d4d524546, 2, 5, 2, 0, 256, 1, 0, 2048, 262144) and
                ref["home"] == "guest-node-1" and ref["owner"] == ref["producer"] == 1 and
                ref["key"] == mappings[0]["key"] and ref["version"] > 0 and ref["incarnation"] > 0 and
                ref["generation"] == number(mappings[0], "generation") and
                ref["checksum"] == int(checksum64(hidden), 16), "model V2 reference mismatch")
        references.append(refs[-1]["reference"])
        done = records(text, "MANAGED_PTO_RESULT")
        require(len(done) == 1 and done[0]["status"] == "pass" and
                done[0]["key"] == mappings[0]["key"] and
                done[0]["generation"] == mappings[0]["generation"] and
                all(done[0][key] == "1" for key in
                    ("mapping_closed", "holder_released", "readonly_inputs", "reference_inputs")) and
                done[0]["compute_leases"] == "0" and done[0]["objects"] == "2",
                "model resources not drained")
        drained = [r for r in records(text, "MANAGED_OWNER") if r["state"] == "drained"]
        require(len(drained) == 2 and {r["object"] for r in drained} == {"input", "output"} and
                all(r[key] == "0" for r in drained for key in
                    ("cpu_pins", "compute_pins", "in_flight_pins", "mapping_pending", "holder_pending")),
                "model owner obligations remain")
        rows = records(text, "MANAGED_MODEL")
        require([r["action"] for r in rows] ==
                (["submitted", "completed", "cpu-verify"] if node == 0 else ["cpu-verify"]),
                "model completion/visibility sequence differs")
        verified = rows[-1]
        require(number(verified, "bytes") == output_bytes and
                int(verified["checksum"], 16) == output_checksum and
                verified["bounds"] == verified["status"] == "pass", "model output oracle mismatch")
        lines = text.splitlines()
        verified_at = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_MODEL ")
                           and "action=cpu-verify" in line.split())
        drained_at = [i for i, line in enumerate(lines) if line.startswith("MANAGED_OWNER ")
                      and "state=drained" in line.split()]
        done_at = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_RESULT "))
        require(verified_at < min(drained_at) <= max(drained_at) < done_at,
                "model resources released before output visibility check")
        for metric in ("put_object_count", "materialize_object_count"):
            require(re.findall(rf"^{metric}=([0-9]+)$", text, re.M) == ["0"],
                    "model payload traversed control plane")
        validate_kernel_regions(text)
    require(references[0] == references[1], "model reader used another V2")
    for metric in ("live_objects", "backing_allocated_bytes", "address_reserved_bytes",
                   "export_mappings", "import_mappings", "live_refs", "in_flight",
                   "quarantined_objects", "quarantined_bytes"):
        require(re.findall(rf"^{metric}=([0-9]+)$", guests[1], re.M) == ["0"],
                f"model home resource ledger not drained: {metric}")
    for home, peer in zip(maps[1], maps[0]):
        require(all(home[key] == peer[key] for key in ("key", "generation", "address")),
                "model guests mapped different allocations")
    submitted, completed, _ = records(guests[0], "MANAGED_MODEL")
    request = submitted["op_id"]
    require(submitted["callable"] == "4" and submitted["inputs"] == "1" and
            submitted["compute_leases"] == "3" and completed["op_id"] == request and
            completed["status"] == "success" and completed["original_completion"] == "1",
            "model original compute completion missing")
    require(not re.search(r"QEMU_UB_GM_.*(?: denied |REJECT |FAILURE_COMPLETION |TIMEOUT |METADATA_CRC_FAIL )", qemu),
            "model provider rejected access")
    bound = records(qemu, "QEMU_UB_GM_GSVA_BIND")
    registers, unbinds = [records(qemu, prefix) for prefix in
                          ("SIM_QEMU_UB_GM_BIND_REGISTER", "QEMU_UB_GM_UNBIND")]
    inputs, outputs = [records(qemu, "QEMU_UB_GM_" + role + "_AUTHORIZE")
                       for role in ("INPUT", "OUTPUT")]
    loads, stores, fences = [records(qemu, "QEMU_UB_GM_" + role)
                             for role in ("LOAD", "STORE", "FENCE")]
    require(tuple(map(len, (bound, registers, unbinds, inputs, outputs, loads, stores, fences))) ==
            (3, 1, 1, 2, 1, 2, 1, 1), "model provider callback counts differ")
    require(all(row["request"] == request for group in
                (bound, registers, unbinds, inputs, outputs, loads, stores, fences) for row in group),
            "model provider request identity differs")
    validate_backing_identities(bound, maps[0])
    validate_home_exports(guests[1], maps[1], qemu)
    auth = inputs + outputs
    requester = number(maps[0][0], "requester_cna")
    require(requester > 0 and requester == number(maps[0][1], "requester_cna"), "invalid model requester")
    for row, binding, length, mapping in zip(auth, (1, 2, 3),
            (8768, 2048, output_bytes), (maps[0][1], maps[0][0], maps[0][1])):
        require(number(row, "binding") == binding and number(row, "length") == length and
                number(row, "requester_cna") == requester and
                number(bound[binding-1], "binding") == binding and
                number(bound[binding-1], "home_va") == number(mapping, "address"),
                "model provider authorization mismatch")
        callbacks = loads[binding-1:binding] if binding < 3 else stores + fences
        for callback in callbacks:
            require(callback["binding"] == row["binding"] and number(callback, "length") == length and
                    number(callback, "addr") == number(row, "local"), "model callback range mismatch")
        for callback in (loads[binding-1:binding] if binding < 3 else stores):
            require(all(number(callback, key) == number(row, key) for key in ("map", "generation")),
                    "model callback mapping identity mismatch")
    require(number(outputs[0], "local") == number(inputs[0], "local") + 16384,
            "model control/output owner-relative offset lost")
    require(registers[0]["op"] == unbinds[0]["op"] == request and
            registers[0]["bindings"] == unbinds[0]["bindings"] == "3" and
            unbinds[0]["reason"] == "completion_success" and
            tuple(unbinds[0][key] for key in
                  ("load_bytes", "store_bytes", "fences", "segment_payload_staging_bytes")) ==
            ("10816", str(output_bytes), "1", "0"), "model completion accounting mismatch")
    runtime = records(qemu, "qwen3-range-runtime-timing:")
    require(len(runtime) == 1 and runtime[0]["task_id"] == request and
            runtime[0]["node"] == "0" and runtime[0]["layers"] == "[1,2]" and
            runtime[0]["terminal_owner"] == runtime[0]["decode_step"] == "0" and
            runtime[0]["real_forward"] == "1" and int(runtime[0]["output_bytes"]) == output_bytes,
            "actual Qwen layer execution evidence missing")
    ordered = [line.split()[0] for line in qemu.splitlines() if line.startswith((
        "SIM_QEMU_UB_GM_BIND_REGISTER ", "QEMU_UB_GM_LOAD ", "QEMU_UB_GM_STORE ",
        "QEMU_UB_GM_FENCE ", "QEMU_UB_GM_UNBIND ", "qwen3-range-runtime-timing: "))]
    require(ordered == ["SIM_QEMU_UB_GM_BIND_REGISTER", "QEMU_UB_GM_LOAD", "QEMU_UB_GM_LOAD",
                        "qwen3-range-runtime-timing:", "QEMU_UB_GM_STORE", "QEMU_UB_GM_FENCE",
                        "QEMU_UB_GM_UNBIND"], "model callback/completion order differs")
    return {"status": "pass", "scope": "managed-two-guest-qwen-single-layer-probe",
            "nodes": 2, "objects": 2, "dispatches": 1, "load_callbacks": 2,
            "store_callbacks": 1, "fences": 1, "output_bytes": output_bytes,
            "output_checksum": f"{output_checksum:016x}", "reference": references[0],
            "am2_complete": False, "pp_decode_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", required=True, type=Path)
    parser.add_argument("--node-b", required=True, type=Path)
    parser.add_argument("--qemu-a", type=Path)
    parser.add_argument("--qemu-b", type=Path)
    parser.add_argument("--home-node", type=int, choices=(0, 1))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--model-probe-bytes", type=lambda value: int(value, 0))
    parser.add_argument("--model-probe-checksum", type=lambda value: int(value, 0))
    parser.add_argument("--require-timeout-recovery", action="store_true")
    parser.add_argument("--access-threads", type=int, choices=range(9))
    parser.add_argument("--concurrent-requests", type=int, choices=(1, 2), default=1)
    parser.add_argument("--require-negative-checks", action="store_true")
    parser.add_argument("--require-readonly-inputs", action="store_true")
    parser.add_argument("--require-reference-inputs", action="store_true")
    parser.add_argument("--layout", choices=GEOMETRIES, default="cross-page")
    parser.add_argument("--strict-io-version", type=int, choices=(1, 2), default=1,
                        help="strict wire version of the recorded QEMU build (default: legacy v1)")
    args = parser.parse_args()
    model_probe = args.model_probe_bytes is not None or args.model_probe_checksum is not None
    if model_probe and (args.model_probe_bytes is None or args.model_probe_checksum is None or
                        args.home_node != 1 or args.layout != "nd" or args.access_threads != 0 or
                        args.concurrent_requests != 1 or not args.require_readonly_inputs or
                        not args.require_reference_inputs or args.require_timeout_recovery or
                        args.require_negative_checks or args.strict_io_version != 2):
        parser.error("model probe requires both oracle fields, home 1, ND readonly V2 and no faults")
    qemu_path = args.qemu_a if args.home_node == 1 else args.qemu_b
    if qemu_path is None:
        parser.error("--qemu-a is required for home 1; --qemu-b is required for home 0")
    paths = [args.node_a, args.node_b, qemu_path]
    try:
        contents = [path.read_bytes() for path in paths]
        text = [data.decode("utf-8", errors="replace") for data in contents]
        result = validate_model_probe(text[:2], text[2], args.model_probe_bytes,
                                      args.model_probe_checksum) if model_probe else validate(
                          text[:2], text[2], args.require_timeout_recovery,
                          args.access_threads, args.require_negative_checks,
                          args.require_readonly_inputs, args.layout, args.home_node,
                          args.concurrent_requests, args.require_reference_inputs,
                          args.strict_io_version)
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
