#!/usr/bin/env python3
"""Validate endpoint resource baselines and live mappings in managed guest logs."""
import argparse
from collections import Counter
from pathlib import Path


def validate(text, node, iterations, size):
    expected = ([0, 1, 1, 0, 0] if node == 0 else [0, 1, 1, 0]) * iterations
    samples = []
    service_imports = None
    for line in text.splitlines():
        if "action=stats status=ok" in line:
            fields = dict(item.split("=", 1) for item in line.split() if "=" in item)
            service_imports = int(fields["import_mappings"])
        if not line.startswith("mem_service provider-resources:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate resource field")
            fields[key] = value
        if (fields.pop("provider"), fields.pop("scope"), fields.pop("version")) != (
                "obmm", "endpoint", "1"):
            raise ValueError("unsupported resource snapshot")
        values = {key: int(value) for key, value in fields.items()}
        live = values["accessible_views"]
        if service_imports is None or (live and service_imports < live):
            raise ValueError("live provider mapping absent from service ledger")
        required = {
            "export_handles": 1, "export_bytes": 2097152,
            # The home maps its existing export FD; only the peer imports.
            "import_handles": live * node, "import_bytes": live * node * 2097152,
            "vma_bytes": live * 2097152, "accessible_bytes": live * size,
            "cleanup_mappings": 0, "closing": 0, "control_close_uncertain": 0,
        }
        if live not in (0, 1) or any(values[key] != value for key, value in required.items()):
            raise ValueError("endpoint resources differ from expected ownership")
        if values["vma_count"] not in ((0,) if not live else (1, 2)):
            raise ValueError("unexpected view/guard VMA count")
        if node == 0 and len(samples) % 5 == 4 and service_imports != 0:
            raise ValueError("service mappings remain after object reclaim")
        samples.append(live)
        service_imports = None
    if samples != expected:
        raise ValueError(f"resource sequence mismatch: {samples}, expected {expected}")


def validate_kernel_regions(text):
    """Compare quiescent sysfs observations; this cannot prove VMA fencing."""
    observations = []
    phase = None
    regions = {}
    for line in text.splitlines():
        if line.startswith("mem_service kernel-regions:"):
            if phase is not None or len(observations) >= 2:
                raise ValueError("overlapping or extra kernel census")
            phase = line.removeprefix("mem_service kernel-regions: phase=")
            if phase != ("baseline", "drained")[len(observations)]:
                raise ValueError("unexpected kernel census phase")
            regions = {}
        elif line.startswith("obmm-kernel-region:"):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            region_id, size = int(fields["id"]), int(fields["bytes"])
            kind = fields["type"]
            if (phase is None or region_id < 0 or region_id in regions or
                    size <= 0 or kind not in ("export", "import")):
                raise ValueError("invalid or duplicate kernel region")
            regions[region_id] = (kind, size)
        elif line.startswith("obmm-kernel-regions:"):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            if (phase is None or fields["version"] != "1" or fields["scope"] != "sysfs" or
                    fields["consistency"] != "non_atomic"):
                raise ValueError("invalid kernel census summary")
            for kind in ("export", "import"):
                sizes = [size for typ, size in regions.values() if typ == kind]
                if (int(fields[f"{kind}_handles"]) != len(sizes) or
                        int(fields[f"{kind}_bytes"]) != sum(sizes)):
                    raise ValueError("kernel census summary differs from region records")
            observations.append(regions)
            phase = None
    if phase is not None or len(observations) != 2 or observations[0] != observations[1]:
        raise ValueError("kernel regions did not return to observed baseline")


def validate_capacity(text, node, iterations):
    """Require saturation with two real mapping lifetimes and a drained ledger."""
    if iterations != 1:
        raise ValueError("capacity profile requires one lifecycle")
    if node != 0:
        marker = ("action=wait_state key=obj-returned-1 status=ok "
                  "state=retired")
        if marker not in text:
            raise ValueError("peer did not observe released capacity barrier")
        return
    if text.count("mem_service capacity-pressure: phase=saturated ") != 1 or \
            text.count("mem_service capacity-pressure: phase=released status=ok") != 1:
        raise ValueError("capacity pressure did not complete exactly once")
    saturated_text, drained_text = text.split(
        "mem_service capacity-pressure: phase=saturated ", 1)[1].split(
        "mem_service capacity-pressure: phase=released status=ok", 1)

    def stats(block, last=False):
        snapshots = block.split("mem_service allocation-stats: status=ok\n")[1:]
        if not snapshots:
            raise ValueError("missing allocation capacity snapshot")
        fields = {}
        for line in (snapshots[-1] if last else snapshots[0]).splitlines():
            name, separator, value = line.partition("=")
            if separator and name in (
                "idempotency_capacity", "idempotency_used", "idempotency_cleanup_reserved",
                "idempotency_available", "idempotency_reservation_deficit",
                "import_mappings", "live_refs", "live_objects", "in_flight",
                "backing_allocated_bytes", "address_reserved_bytes",
            ):
                if name in fields:
                    raise ValueError("duplicate capacity field")
                fields[name] = int(value)
            if name == "idempotency_reservation_deficit":
                return fields
        raise ValueError("incomplete allocation capacity snapshot")

    saturated, drained = stats(saturated_text), stats(drained_text, last=True)
    if (saturated["import_mappings"] != 2 or saturated["live_refs"] != 2 or
            saturated["idempotency_cleanup_reserved"] != 9 or
            saturated["idempotency_used"] + saturated["idempotency_cleanup_reserved"] !=
            saturated["idempotency_capacity"] or
            saturated["idempotency_available"] != 0 or
            saturated["idempotency_reservation_deficit"] != 0):
        raise ValueError("capacity was not exhausted with both mappings protected")
    if any(drained[field] != 0 for field in (
            "import_mappings", "live_refs", "live_objects", "in_flight",
            "backing_allocated_bytes", "address_reserved_bytes",
            "idempotency_cleanup_reserved", "idempotency_available",
            "idempotency_reservation_deficit")) or \
            drained["idempotency_used"] != drained["idempotency_capacity"]:
        raise ValueError("full-table cleanup did not drain all resources")


def validate_sdk_contract(text, node, iterations):
    """Require per-object SDK rejections and actual holder-op replay results."""
    failures, successes = Counter(), Counter()
    completions = 0
    for line in text.splitlines():
        if not line.startswith("mem_service object-session:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate session result field")
            fields[key] = value
        if fields.get("session") != f"client-{node}":
            continue
        if fields.get("result") == "failed" or "mismatch" in fields:
            raise ValueError("managed SDK contract session failed")
        completions += fields.get("result") == "ok"
        if "action" not in fields:
            continue
        key, action, status = fields.get("key", "-"), fields["action"], fields["status"]
        if status == "ok":
            successes[key, action] += 1
        else:
            failures[key, action, status, fields.get("note", "")] += 1
    expected = Counter()
    for iteration in range(1, iterations + 1):
        key = f"obj-share-{iteration}"
        expected["absent-" + key, "acquire", "not_found", ""] = 1
        expected[key, "acquire", "stale_ref", ""] = 1
        expected[key, "write", "unsupported", "not_writable"] = 1
        expected[key, "release", "internal", "mapping_active"] = 1
        expected[key, "read", "checksum_mismatch", "pattern_mismatch"] = 1
        for action in ("read", "write", "publish_data", "wait_visible"):
            expected[key, action, "capacity_exceeded", "out_of_bounds"] = 1
        for action in ("acquire", "release"):
            if successes[key, action] != 2:
                raise ValueError("missing or extra holder operation replay")
    if failures != expected or completions != iterations:
        raise ValueError("managed SDK contract evidence differs from expected rounds")


def validate_map_conflict(text, node, iterations):
    """Require real EEXIST with a retained original mapping and exact remapping."""
    expected = {f"obj-share-{i}" for i in range(1, iterations + 1)}
    probes, mappings, results = Counter(), Counter(), Counter()
    addresses = {}
    pending = None
    completions = 0
    for line in text.splitlines():
        provider = line.startswith("mem_service obmm-map:")
        if not provider and not line.startswith("mem_service object-session:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate conflict evidence field")
            fields[key] = value
        if provider:
            if (pending is not None or fields["result"] != "failed" or
                    fields["stage"] != "mmap" or int(fields["errno"]) != 17):
                raise ValueError("expected exactly one actual mmap EEXIST per probe")
            pending = (int(fields["fixed_va"], 0), int(fields["len"]))
            continue
        if fields.get("session") != f"client-{node}":
            continue
        if (fields.get("result") == "failed" or "mismatch" in fields or
                fields.get("status", "ok") != "ok"):
            raise ValueError("conflict session failed")
        completions += fields.get("result") == "ok"
        key = fields.get("key")
        if "cpu_conflict_probe" in fields:
            base, page = int(fields["base"], 0), int(fields["len"])
            if (fields["cpu_conflict_probe"] != "pass" or key not in expected or
                    fields["preserved"] != "1" or fields["cleanup_pending"] != "0" or
                    fields.get("source") != "retained_handle" or
                    page not in (4096, 16384, 65536) or not base or base % page or
                    mappings[key] != 1 or addresses.get(key) != base or
                    pending is None or pending[0] != base or pending[1] < page):
                raise ValueError("conflict probe lacks matching preserved-page evidence")
            probes[key] += 1
            pending = None
        if fields.get("action") == "probe_conflict":
            results[key] += 1
        if fields.get("action") == "map":
            base = int(fields["base"], 0)
            if key not in expected or pending is not None:
                raise ValueError("unexpected mapping during conflict probe")
            if mappings[key] == 0:
                addresses[key] = base
            elif addresses[key] != base or probes[key] != 1:
                raise ValueError("remapping changed address or preceded conflict probe")
            mappings[key] += 1
    if (pending is not None or completions != iterations or
            probes != Counter({key: 1 for key in expected}) or results != probes or
            mappings != Counter({key: 2 for key in expected})):
        raise ValueError("missing or extra conflict probe/remapping evidence")


def validate_descriptor_contract(text, node, iterations):
    """Require each binding mutation to fail before the original view maps."""
    expected, failures, mappings = Counter(), Counter(), Counter()
    completions = 0
    for iteration in range(1, iterations + 1):
        key = f"obj-share-{iteration}"
        for byte in (0, 4, 8, 12, 16, 24, 32, 40, 44, 48, 56, 64, 68, 72,
                     76, 80, 84, 88, 92, 95):
            expected[key, "descriptor", str(byte)] = 1
        for fault in ("descriptor_length", "descriptor_oversize", "address", "address_len",
                      "size", "alignment", "capabilities", "home", "incarnation"):
            expected[key, fault, ""] = 1
    for line in text.splitlines():
        if line.startswith("mem_service obmm-map:"):
            raise ValueError("binding rejection unexpectedly reached provider mmap failure")
        if not line.startswith("mem_service object-session:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate binding evidence field")
            fields[key] = value
        if fields.get("session") != f"client-{node}":
            continue
        if fields.get("result") == "failed" or "mismatch" in fields:
            raise ValueError("descriptor contract session failed")
        completions += fields.get("result") == "ok"
        if "fault" in fields:
            key = fields.get("key", "")
            if (fields.get("action") != "map" or fields.get("status") != "stale_ref" or
                    fields.get("note") != "map_failed" or mappings[key]):
                raise ValueError("binding mutation did not fail before ordinary mapping")
            failures[key, fields["fault"], fields.get("fault_byte", "")] += 1
        elif fields.get("status", "ok") != "ok":
            raise ValueError("unexpected descriptor contract rejection")
        elif fields.get("action") == "map":
            mappings[fields.get("key", "")] += 1
    if (failures != expected or completions != iterations or
            mappings != Counter({f"obj-share-{i}": 2 for i in range(1, iterations + 1)})):
        raise ValueError("missing or extra descriptor binding evidence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--node", type=int, choices=(0, 1), required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    faults = parser.add_mutually_exclusive_group()
    faults.add_argument("--sdk-contract", action="store_true")
    faults.add_argument("--map-conflict", action="store_true")
    faults.add_argument("--descriptor-contract", action="store_true")
    faults.add_argument("--capacity", action="store_true")
    args = parser.parse_args()
    if args.iterations < 1 or not 0 < args.size <= 2097152:
        parser.error("positive iterations and 1..2097152 logical bytes required")
    try:
        text = args.log.read_text()
        validate(text, args.node, args.iterations, args.size)
        validate_kernel_regions(text)
        if args.sdk_contract:
            validate_sdk_contract(text, args.node, args.iterations)
        if args.map_conflict:
            validate_map_conflict(text, args.node, args.iterations)
        if args.descriptor_contract:
            validate_descriptor_contract(text, args.node, args.iterations)
        if args.capacity:
            validate_capacity(text, args.node, args.iterations)
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"mapping resource validation failed: {error}\n")
    print(f"mapping_resources=pass node={args.node} iterations={args.iterations}")
    print(f"kernel_regions=pass node={args.node} consistency=non_atomic")
    if args.sdk_contract:
        print(f"sdk_contract=pass node={args.node} iterations={args.iterations}")
    if args.map_conflict:
        print(f"map_conflict=pass node={args.node} iterations={args.iterations}")
    if args.descriptor_contract:
        print(f"descriptor_contract=pass node={args.node} iterations={args.iterations}")
    if args.capacity:
        print(f"capacity_cleanup=pass node={args.node} iterations={args.iterations}")


if __name__ == "__main__":
    main()
