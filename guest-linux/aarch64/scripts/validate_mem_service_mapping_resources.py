#!/usr/bin/env python3
"""Validate endpoint resource baselines and live mappings in managed guest logs."""
import argparse
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--node", type=int, choices=(0, 1), required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    args = parser.parse_args()
    if args.iterations < 1 or not 0 < args.size <= 2097152:
        parser.error("positive iterations and 1..2097152 logical bytes required")
    try:
        validate(args.log.read_text(), args.node, args.iterations, args.size)
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"mapping resource validation failed: {error}\n")
    print(f"mapping_resources=pass node={args.node} iterations={args.iterations}")


if __name__ == "__main__":
    main()
