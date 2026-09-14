#!/usr/bin/env python3
"""Cross-check actual managed V2 writer/reader guest logs and resource drain."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from validate_mem_service_mapping_resources import validate_kernel_regions


def require(condition, message):
    if not condition:
        raise ValueError(message)


def records(text, prefix):
    rows = []
    for line in text.splitlines():
        if not line.startswith(prefix + " "):
            continue
        row = {}
        for item in line[len(prefix) + 1:].split():
            require("=" in item, f"malformed {prefix} record")
            name, value = item.split("=", 1)
            require(name not in row, f"duplicate field {name}")
            row[name] = value
        rows.append(row)
    return rows


def checksum(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def decode_reference(encoded):
    require(len(encoded) == 512, "V2 reference must have 512 hex digits")
    raw = bytes.fromhex(encoded)
    require(len(raw) == 256, "V2 reference must contain 256 bytes")
    names = ("magic", "layout", "kind", "state", "flags", "owner", "producer",
             "version", "key_hash", "offset", "length", "checksum", "wire_bytes",
             "access", "generation", "incarnation", "size", "key", "home")
    values = dict(zip(names, struct.unpack("<QHHHHIIQQQQQIIQQQ96s64s", raw)))
    for name in ("key", "home"):
        value, separator, padding = values[name].partition(b"\0")
        require(separator and value and not any(padding), "noncanonical V2 identity")
        require(all(32 < byte < 127 and byte != 61 for byte in value), "invalid V2 token")
        values[name] = value.decode("ascii")
    require(values["key_hash"] == checksum(values["key"].encode()), "wrong V2 key hash")
    return values


def expected_operations(node, key):
    ready, done = key.replace("obj-ref", "ref-ready"), key.replace("obj-ref", "ref-done")
    if node == 0:
        operations = [(action, key, "ok") for action in
                      ("wait_state", "acquire", "begin_reference", "stats", "map", "stats",
                       "write", "write", "read", "read")]
        operations += [("publish_reference", key + "/a", "ok"),
                       ("publish_reference", key + "/b", "ok"),
                       ("write", key, "unsupported")]
        operations += [(action, key, "ok") for action in ("unmap", "stats", "seal_reference", "release")]
        operations += [("allocate", ready, "ok"), ("wait_state", done, "ok")]
        for retired in (key, ready, done):
            operations += [("retire", retired, "ok"), ("wait_state", retired, "ok")]
        operations += [("stats", key, "ok")]
    else:
        operations = [("wait_state", ready, "ok"), ("stats", key, "ok")]
        for view in ("a", "b"):
            operations += [("acquire_reference", key + "/" + view, "ok")]
            operations += [(action, key, "ok") for action in
                           ("map_reference", "stats", "wait_visible", "read", "probe_readonly")]
            operations += [("read", key, "capacity_exceeded")]
            operations += [(action, key, "ok") for action in ("unmap", "stats", "release")]
        operations += [("allocate", done, "ok"), ("wait_state", done, "ok"), ("stats", key, "ok")]
    return operations


def validate_resources(text, node, iterations):
    snapshots = records(text, "mem_service provider-resources:")
    pattern = [0, 65536, 0, 0] if node == 0 else [0, 512, 0, 512, 0, 0]
    require(len(snapshots) == len(pattern) * iterations, "missing provider resource snapshots")
    for row, size in zip(snapshots, pattern * iterations):
        live = int(size != 0)
        require((row["provider"], row["scope"], row["version"]) == ("obmm", "endpoint", "1"),
                "wrong provider resource domain")
        expected = dict(export_handles=1, export_bytes=2097152,
                        import_handles=node * live, import_bytes=node * live * 2097152,
                        vma_bytes=live * 2097152, accessible_views=live, accessible_bytes=size,
                        cleanup_mappings=0, closing=0, control_close_uncertain=0)
        require(all(int(row[name]) == value for name, value in expected.items()),
                "provider ownership or view geometry differs")
        require(int(row["vma_count"]) in ((1, 2, 3) if live else (0,)), "wrong VMA count")
    validate_kernel_regions(text)


def validate(node_a, node_b, iterations, seed, incarnation):
    require(iterations in (1, 2) and incarnation > 0 and 0 <= seed < (1 << 64) - 1,
            "unsupported reference guest configuration")
    results = []
    all_ops = []
    for node, text in enumerate((node_a, node_b)):
        rows = records(text, "mem_service object-session:")
        require(rows and all(row.get("session") == f"client-{node}" for row in rows),
                "wrong SDK session identity")
        require(not any("mismatch" in row or row.get("result") == "failed" or
                        "unreleased_holder" in row or "cleanup_pending" in row for row in rows),
                "failed SDK session or retained resources")
        endings = [row for row in rows if "result" in row]
        require(len(endings) == iterations and all(row["result"] == "ok" for row in endings),
                "missing successful session completion")
        require(sum(row.get("peer_canary") == "ok" and row.get("peers") == "1" for row in rows)
                == iterations, "missing endpoint peer canary")
        if node == 1:
            require(not any("reference_hex" in row for row in rows), "reader must not publish references")
        ops = [row for row in rows if "op" in row]
        per_round = len(expected_operations(node, "obj-ref-1"))
        require(len(ops) == per_round * iterations, "wrong operation count")
        for round_index in range(iterations):
            part = ops[round_index * per_round:(round_index + 1) * per_round]
            expected = expected_operations(node, f"obj-ref-{round_index + 1}")
            require(int(endings[round_index]["ops"]) == per_round, "wrong session completion count")
            for index, (row, (action, key, status)) in enumerate(zip(part, expected), 1):
                require(int(row["op"]) == index and row["action"] == action and row["status"] == status,
                        "operation sequence or result mismatch")
                if action != "stats":
                    require(row["key"] == key, "wrong operation key")
                if action == "wait_state":
                    retired = node == 0 and index > 19 or node == 1 and index == per_round - 1
                    require(row["state"] == ("retired" if retired else "active"), "wrong barrier state")
        stats = [row for row in ops if row["action"] == "stats"]
        pattern = [0, 1, 0, 0] if node == 0 else [0, 1, 0, 1, 0, 0]
        require([int(row["import_mappings"]) for row in stats] == pattern * iterations,
                "service mapping ledger disagrees with access sequence")
        for field in ("live_objects", "live_refs", "in_flight", "import_mappings",
                      "backing_allocated_bytes", "address_reserved_bytes", "quarantined_objects", "quarantined_bytes"):
            require(int(stats[-1][field]) == 0, f"service resources not drained: {field}")
        require(f"status=ok node={node} role={'provider' if node == 0 else 'consumer'} backend=obmm" in text,
                "missing guest completion")
        validate_resources(text, node, iterations)
        all_ops.append(ops)

    receipts = records(node_a, "mem_service reference-allocation:")
    published = [row for row in records(node_a, "mem_service object-session:") if "reference_hex" in row]
    require(len(receipts) == iterations and len(published) == 2 * iterations, "missing publication identities")
    reader_faults = [row for row in records(node_b, "mem_service object-session:") if "cpu_fault_probe" in row]
    require(len(reader_faults) == 2 * iterations and
            all(row["cpu_fault_probe"] == "probe_readonly" and row["offset"] == "0" and
                row["signal"] in ("7", "11") for row in reader_faults), "readonly CPU protection unproven")
    generations = []
    for round_index, receipt in enumerate(receipts, 1):
        key = f"obj-ref-{round_index}"
        require(receipt["key"] == key and int(receipt["generation"]) > 0 and int(receipt["version"]) > 0,
                "invalid allocation receipt")
        generation = int(receipt["generation"])
        generations.append(generation)
        writer = [row for row in all_ops[0] if row.get("key") == key]
        reader = [row for row in all_ops[1] if row.get("key") == key]
        for row in writer + reader:
            if "generation" in row:
                require(int(row["generation"]) == generation, "operation allocation generation mismatch")
        maps = [row for row in writer if row["action"] == "map"]
        require(len(maps) == 1 and int(maps[0]["len"]) == 65536, "writer must map its full allocation")
        base = int(maps[0]["base"], 0)
        require(base > 0, "invalid writer address")
        frozen = [row for row in writer if row["action"] == "write" and row["status"] == "unsupported"]
        require(len(frozen) == 1 and frozen[0].get("note") == "reference_payload_frozen", "writer was not frozen")
        reader_maps = [row for row in reader if row["action"] == "map_reference"]
        visible = [row for row in reader if row["action"] == "wait_visible"]
        reads = [row for row in reader if row["action"] == "read" and row["status"] == "ok"]
        bounds = [row for row in reader if row["action"] == "read" and row["status"] == "capacity_exceeded"]
        require(len(bounds) == 2 and all(row.get("note") == "out_of_bounds" for row in bounds),
                "reader bounds rejection unproven")
        for view_index, (view, offset) in enumerate((("a", 128), ("b", 4224))):
            publication = published[(round_index - 1) * 2 + view_index]
            require(publication["reference_key"] == key + "/" + view, "wrong logical publication")
            ref = decode_reference(publication["reference_hex"])
            expected_checksum = checksum(bytes((seed + view_index + i) & 255 for i in range(512)))
            expected = dict(magic=0x514f424d4d524546, layout=2, kind=5, state=2, flags=0,
                            owner=0, producer=0, version=int(receipt["version"]) + 1, offset=offset,
                            length=512, checksum=expected_checksum, wire_bytes=256, access=1,
                            generation=generation, incarnation=incarnation, size=65536,
                            key=key, home="guest-node-0")
            require(all(ref[name] == value for name, value in expected.items()), "V2 identity or payload mismatch")
            require(int(reader_maps[view_index]["base"], 0) == base + offset and
                    int(reader_maps[view_index]["len"]) == 512, "reader mapped a different address or range")
            for row in (visible[view_index], reads[view_index]):
                require(int(row["offset"]) == 0 and int(row["len"]) == 512 and
                        int(row["checksum"], 0) == expected_checksum, "peer payload visibility/checksum mismatch")
            local = [row for row in writer if row["action"] in ("write", "read") and
                     row["status"] == "ok" and int(row["offset"]) == offset]
            require(len(local) == 2 and all(int(row["len"]) == 512 and
                    int(row["checksum"], 0) == expected_checksum for row in local),
                    "writer bytes/checksum mismatch")
            acquires = [row for row in all_ops[1] if row["action"] == "acquire_reference" and row["key"] == key + "/" + view]
            require(len(acquires) == 1 and int(acquires[0]["generation"]) == generation,
                    "reader acquired a different generation")
            results.append(dict(key=key + "/" + view, generation=generation, offset=offset,
                                bytes=512, checksum=expected_checksum, address=base + offset))
    require(len(set(generations)) == iterations, "allocation generation reused across rounds")
    return dict(status="pass", scope="managed-reference-two-guest", iterations=iterations,
                views=results, node_a_sha256=hashlib.sha256(node_a.encode()).hexdigest(),
                node_b_sha256=hashlib.sha256(node_b.encode()).hexdigest())


def validate_qemu(text, views):
    """Require strict managed routes for the exact reader views, then unmap."""
    expected = [view["address"] - view["offset"] for view in views]
    seen = set()
    active = None
    routes = []
    for line in text.splitlines():
        opening = line.startswith("GSVA_MAP: map_id=")
        closing = line.startswith("GSVA_UNMAP: map_id=")
        if not opening and not closing:
            continue
        prefix = "GSVA_MAP:" if opening else "GSVA_UNMAP:"
        row = records(line, prefix)[0]
        identity = tuple(int(row[name], 0) for name in
                         ("map_id", "segment_id", "home_va", "epoch"))
        if opening:
            require(active is None and identity[0] not in seen and
                    all(value > 0 for value in identity), "invalid or overlapping strict route")
            require(len(routes) < len(expected) and identity[2] == expected[len(routes)],
                    "strict route does not match reader allocation")
            require(int(row["size"], 0) == 2097152 and row["source"] == "2" and
                    row["profile"] == "1", "wrong managed route size or address profile")
            seen.add(identity[0])
            active = identity
            routes.append(dict(map_id=identity[0], segment_id=identity[1],
                               home_va=identity[2], epoch=identity[3]))
        else:
            require(active == identity and row["tombstone"] == "no",
                    "strict route cleanup identity mismatch")
            active = None
    require(active is None and len(routes) == len(expected), "incomplete strict route lifecycle")
    require(text.count("GSVA_MAP: cpu_window registered ") == len(expected) and
            text.count("GSVA_UNMAP: cpu_window removed ") == len(expected),
            "incomplete reader CPU windows")
    return routes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", type=Path, required=True)
    parser.add_argument("--node-b", type=Path, required=True)
    parser.add_argument("--qemu-b", type=Path, required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--incarnation", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(args.node_a.read_text(), args.node_b.read_text(), args.iterations, args.seed, args.incarnation)
        qemu = args.qemu_b.read_text()
        result["reader_routes"] = validate_qemu(qemu, result["views"])
        result["qemu_b_sha256"] = hashlib.sha256(qemu.encode()).hexdigest()
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, KeyError, ValueError, IndexError, struct.error) as error:
        parser.exit(1, f"managed reference validation failed: {error}\n")
    print(f"managed_reference=pass iterations={args.iterations} views={len(result['views'])}")


if __name__ == "__main__":
    main()
