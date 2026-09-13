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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--node", type=int, choices=(0, 1), required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--sdk-contract", action="store_true")
    args = parser.parse_args()
    if args.iterations < 1 or not 0 < args.size <= 2097152:
        parser.error("positive iterations and 1..2097152 logical bytes required")
    try:
        text = args.log.read_text()
        validate(text, args.node, args.iterations, args.size)
        validate_kernel_regions(text)
        if args.sdk_contract:
            validate_sdk_contract(text, args.node, args.iterations)
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"mapping resource validation failed: {error}\n")
    print(f"mapping_resources=pass node={args.node} iterations={args.iterations}")
    print(f"kernel_regions=pass node={args.node} consistency=non_atomic")
    if args.sdk_contract:
        print(f"sdk_contract=pass node={args.node} iterations={args.iterations}")


if __name__ == "__main__":
    main()
