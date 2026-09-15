#!/usr/bin/env python3
"""Validate real two-generation descriptor retirement, reuse, and resource drain."""
import argparse
import hashlib
import json
from pathlib import Path

from validate_mem_service_reference_guest import checksum, records, require
from validate_mem_service_mapping_resources import validate_kernel_regions


def expected_operations(node):
    result = []
    for key in ("retired-old", "retired-new"):
        if node == 0:
            result += [(action, key) for action in ("allocate", "wait_state", "acquire",
                       "map", "write", "publish_data", "read", "unmap", "release")]
            result += [("allocate", key + "-ready"), ("wait_state", key + "-done")]
            for marker in (key, key + "-ready", key + "-done"):
                result += [("retire", marker), ("wait_state", marker)]
        else:
            result += [("wait_state", key + "-ready")]
            result += [(action, key) for action in ("acquire", "map", "wait_visible", "read")]
            if key == "retired-old":
                result += [("capture_mapping", key)]
            result += [("unmap", key)]
            if key == "retired-new":
                result += [("probe_retired_mapping", "retired-old"), ("stats", "-")]
                result += [(action, key) for action in ("map", "wait_visible", "read", "unmap")]
            result += [("release", key), ("allocate", key + "-done"),
                       ("wait_state", key + "-done")]
    return result + [("stats", "-")]


def validate(node_a, node_b, qemu_b, seed):
    require(0 <= seed < (1 << 64) - 1, "invalid seed")
    generations, addresses = {}, []
    capture = probe = None
    for node, text in enumerate((node_a, node_b)):
        rows = records(text, "mem_service object-session:")
        require(rows and all(row.get("session") == f"retired-client-{node}" for row in rows),
                "wrong session identity")
        require(not any("mismatch" in row or "cleanup_pending" in row or
                        "unreleased_holder" in row or row.get("result") == "failed"
                        for row in rows), "failed session or retained ownership")
        ops = [row for row in rows if "op" in row]
        expected = expected_operations(node)
        require(len(ops) == len(expected), "wrong operation count")
        for index, (row, (action, key)) in enumerate(zip(ops, expected), 1):
            require(row.get("action") == action and row.get("status") == "ok" and
                    int(row["op"]) == index and (action == "stats" or row["key"] == key),
                    "operation ordering, identity or status mismatch")
            if action == "wait_state":
                retired = (node == 1 and key.endswith("-done")) or (
                    node == 0 and index > 1 and ops[index - 2]["action"] == "retire")
                require(row["state"] == ("retired" if retired else "active"),
                        "barrier state mismatch")
            if action == "acquire":
                generation = int(row["generation"])
                require(generation > 0 and generation == generations.setdefault(key, generation),
                        "generation differs between participants")
            if action in ("read", "write", "publish_data", "wait_visible"):
                pattern_seed = (seed + int(key == "retired-new")) % 256
                pattern = bytes((pattern_seed + i) % 256 for i in range(65536))
                require(row["offset"] == "0" and row["len"] == "65536" and
                        int(row["checksum"], 0) == checksum(pattern), "payload differs")
        endings = [row for row in rows if "result" in row]
        require(len(endings) == 1 and endings[0].get("result") == "ok" and
                int(endings[0]["ops"]) == len(expected), "missing session completion")
        require(sum(row.get("peer_canary") == "ok" and row.get("peers") == "1"
                    for row in rows) == 1, "missing peer canary")
        maps = [row for row in ops if row["action"] == "map"]
        for row in maps:
            require(row["len"] == "65536" and int(row["base"], 0) > 0, "wrong map geometry")
            addresses.append(int(row["base"], 0))
        faults = [(index, row) for index, row in enumerate(rows) if "cpu_unmapped_probe" in row]
        require(len(faults) == len(maps), "missing unmap faults")
        for mapping, (index, row) in zip(maps, faults):
            require(index + 1 < len(rows) and rows[index + 1].get("action") == "unmap" and
                    rows[index + 1]["key"] == row["key"] == mapping["key"] and
                    row["cpu_unmapped_probe"] == "pass" and row["signal"] in ("7", "11") and
                    row["read_before_unmap"] == row["unmap_confirmed"] == "1" and
                    int(row["address"], 0) == int(mapping["base"], 0) and
                    int(row["generation"]) == generations[row["key"]], "invalid unmap fault")
        captures = [(index, row) for index, row in enumerate(rows) if "mapping_capture" in row]
        probes = [(index, row) for index, row in enumerate(rows) if "retired_mapping_probe" in row]
        require(len(captures) == len(probes) == node, "missing or misplaced saved descriptor probe")
        if node:
            ci, capture = captures[0]
            pi, probe = probes[0]
            require(ci < pi and rows[ci + 1].get("action") == "capture_mapping" and
                    rows[pi + 1].get("action") == "probe_retired_mapping", "probe receipt ordering")
            require(capture["mapping_capture"] == "ok" and capture["key"] == "retired-old" and
                    capture["len"] == "65536" and int(capture["descriptor_checksum"], 0) > 0,
                    "invalid captured identity")
            require(probe["retired_mapping_probe"] == "pass" and probe["key"] == "retired-old" and
                    probe["replacement_key"] == "retired-new" and probe["map_result"] == "0" and
                    probe["signal"] in ("7", "11") and probe["unmap_confirmed"] == "1",
                    "old descriptor access rejection or cleanup unproven")
        stats = [row for row in ops if row["action"] == "stats"]
        for field in ("live_objects", "live_refs", "in_flight", "import_mappings",
                      "backing_allocated_bytes", "address_reserved_bytes",
                      "quarantined_objects", "quarantined_bytes"):
            require(stats[-1][field] == "0", f"resource retained: {field}")
        snapshots = records(text, "mem_service provider-resources:")
        require(len(snapshots) == len(stats), "missing endpoint resource snapshots")
        for row in snapshots:
            require((row["provider"], row["scope"], row["version"]) == ("obmm", "endpoint", "1"),
                    "wrong resource snapshot domain")
            for field in ("import_handles", "import_bytes", "vma_count", "vma_bytes",
                          "accessible_views", "accessible_bytes", "cleanup_mappings",
                          "closing", "control_close_uncertain"):
                require(row[field] == "0", f"endpoint retained: {field}")
            require(row["export_handles"] == "1" and row["export_bytes"] == "2097152",
                    "endpoint canary missing")
        validate_kernel_regions(text)
        require(f"status=ok node={node} role={'provider' if node == 0 else 'consumer'} backend=obmm"
                in text, "missing guest completion")
    require(len(set(addresses)) == 1 and generations["retired-old"] != generations["retired-new"],
            "same-address generation reuse unproven")
    require(int(capture["address"], 0) == int(probe["address"], 0) == addresses[0] and
            int(capture["generation"]) == int(probe["generation"]) == generations["retired-old"] and
            int(probe["replacement_generation"]) == generations["retired-new"], "probe identity mismatch")
    routes, active = [], None
    for line in qemu_b.splitlines():
        if line.startswith("GSVA_MAP: map_id="):
            row = records(line, "GSVA_MAP:")[0]
            require(active is None and row["source"] == "2" and row["profile"] == "1" and
                    int(row["home_va"], 0) == addresses[0] and int(row["size"], 0) == 2097152,
                    "wrong or overlapping provider route")
            active = tuple(int(row[name], 0) for name in ("map_id", "segment_id", "home_va", "epoch"))
            routes.append(active)
        elif line.startswith("GSVA_UNMAP: map_id="):
            row = records(line, "GSVA_UNMAP:")[0]
            require(active == tuple(int(row[name], 0) for name in
                    ("map_id", "segment_id", "home_va", "epoch")) and row["tombstone"] == "no",
                    "provider route cleanup mismatch")
            active = None
    require(active is None and len(routes) == 4 and len({row[0] for row in routes}) == 4 and
            all(all(value > 0 for value in row) for row in routes) and
            routes[0][1:] == routes[2][1:] and routes[1][1:] == routes[3][1:] and
            routes[0][1] != routes[1][1], "saved/new provider route lifetimes unproven")
    require(qemu_b.count("GSVA_MAP: cpu_window registered ") == 4 and
            qemu_b.count("GSVA_UNMAP: cpu_window removed ") == 4, "CPU windows not drained")
    return dict(status="pass", scope="actual-retired-descriptor-cpu-read", generations=generations,
                address=addresses[0], reader_routes=routes, capture=capture, probe=probe,
                node_a_sha256=hashlib.sha256(node_a.encode()).hexdigest(),
                node_b_sha256=hashlib.sha256(node_b.encode()).hexdigest(),
                qemu_b_sha256=hashlib.sha256(qemu_b.encode()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("node-a", "node-b", "qemu-b", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--seed", type=int, required=True)
    args = parser.parse_args()
    try:
        result = validate(args.node_a.read_text(), args.node_b.read_text(), args.qemu_b.read_text(), args.seed)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, KeyError, ValueError, IndexError) as error:
        parser.exit(1, f"managed retired validation failed: {error}\n")
    print("managed_retired=pass generations=2 saved_descriptor_checks=1")


if __name__ == "__main__":
    main()
