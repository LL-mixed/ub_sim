#!/usr/bin/env python3
"""Validate repeated Memory Service manager crashes in two guest logs."""
import argparse
import hashlib
import json
from pathlib import Path

from validate_mem_service_reference_guest import records, require


FAULT = "manager_crash"
KEY = "recovery-fault-manager-crash"


def one_record(text, prefix, required):
    rows = records(text, prefix)
    matches = [row for row in rows
               if all(row.get(name) == value for name, value in required.items())]
    require(len(matches) == 1, f"expected one matching {prefix} record")
    return matches[0]


def kernel_snapshots(text):
    snapshots = []
    phase = None
    regions = {}
    for line in text.splitlines():
        if line.startswith("mem_service kernel-regions:"):
            require(phase is None, "overlapping kernel census")
            phase = line.removeprefix("mem_service kernel-regions: phase=")
            require(phase in ("baseline", "isolated"), "unexpected kernel census phase")
            regions = {}
        elif line.startswith("obmm-kernel-region:"):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            region_id = int(fields["id"])
            size = int(fields["bytes"])
            kind = fields["type"]
            require(phase is not None and region_id not in regions and size > 0 and
                    kind in ("export", "import"), "invalid kernel region")
            regions[region_id] = (kind, size)
        elif line.startswith("obmm-kernel-regions:"):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            require(phase is not None and fields.get("version") == "1" and
                    fields.get("scope") == "sysfs" and
                    fields.get("consistency") == "non_atomic",
                    "invalid kernel census summary")
            for kind in ("export", "import"):
                sizes = [size for typ, size in regions.values() if typ == kind]
                require(int(fields[f"{kind}_handles"]) == len(sizes) and
                        int(fields[f"{kind}_bytes"]) == sum(sizes),
                        "kernel census summary differs from records")
            snapshots.append((phase, regions.copy()))
            phase = None
    require(phase is None and [name for name, _ in snapshots] ==
            ["baseline", "isolated"], "missing manager recovery kernel census")
    return snapshots[0][1], snapshots[1][1]


def validate(node_a, node_b):
    for node, text in enumerate((node_a, node_b)):
        require("mem_service object-share: status=fail" not in text,
                "guest reported object-share failure")
        require(f"mem_service object-share: status=ok node={node} "
                f"role={'provider' if node == 0 else 'consumer'} backend=obmm" in text,
                "missing guest completion")
        require("fault-injection=triggered" not in text,
                "manager crash used worker fault injection")

    active = one_record(node_a, "mem_service manager-recovery:", {
        "node": "0", "phase": "active", "status": "ready", "fault": FAULT,
        "key": KEY, "live_refs": "1",
    })
    holder = one_record(node_b, "mem_service manager-recovery:", {
        "node": "1", "phase": "holder", "status": "ready", "fault": FAULT,
        "key": KEY, "live_refs": "1",
    })
    generation = int(active["generation"])
    require(generation > 0 and int(holder["generation"]) == generation,
            "holder observed a different allocation generation")

    exits = records(node_a, "mem_service manager-recovery:")
    exits = [row for row in exits if row.get("phase") == "manager-exit"]
    require(len(exits) == 2, "expected two manager exits")
    for restart, row in enumerate(exits, 1):
        require(row == {
            "node": "0", "phase": "manager-exit", "status": "expected",
            "restart": str(restart), "signal": "9", "exit_code": "137",
        }, "manager exit receipt differs")

    for node, text in enumerate((node_a, node_b)):
        worker = one_record(text, "mem_service manager-recovery:", {
            "node": str(node), "phase": "worker-exit", "status": "expected",
        })
        require(len(worker) == 4 and int(worker["exit_code"]) != 0,
                "provider worker did not fail closed with the manager")

    transitions = [row for row in records(node_b, "mem_service manager-recovery:")
                   if row.get("phase") == "manager-transition"]
    require([(row.get("status"), row.get("restart")) for row in transitions] ==
            [("down", "1"), ("up", "1"), ("down", "2"), ("up", "2")],
            "peer did not observe both manager restart cycles")
    require(all(len(row) == 4 and row.get("node") == "1" for row in transitions),
            "unexpected manager transition fields")

    restarts = [row for row in records(node_a, "mem_service manager-recovery:")
                if row.get("phase") == "restart"]
    require(len(restarts) == 2, "expected two isolated restart receipts")
    for restart, row in enumerate(restarts, 1):
        expected = {
            "node": "0", "phase": "restart", "status": "isolated",
            "restart": str(restart), "key": KEY, "generation": str(generation),
            "state": "quarantined", "provider_backed": "1", "live_refs": "1",
            "identity_preserved": "1", "managed_recovery_required": "1",
        }
        require(row == expected, "restart did not preserve the quarantined identity")

    directory = one_record(node_a, "mem_service manager-recovery:", {
        "node": "0", "phase": "directory", "status": "isolated",
        "provider_active_count": "2", "provider_directory_ready": "1",
        "data_plane_ready": "0",
    })
    require(len(directory) == 6, "unexpected provider directory evidence")
    for phase in ("old-replay", "new-allocation"):
        rejected = one_record(node_a, "mem_service manager-recovery:", {
            "node": "0", "phase": phase, "status": "rejected",
            "reason": "managed_reconciliation_required",
        })
        require((phase == "old-replay" and rejected.get("key") == KEY and
                 int(rejected.get("generation", "0")) == generation) or
                (phase == "new-allocation" and len(rejected) == 4),
                "unsafe work was not rejected after manager recovery")

    home_observation = one_record(node_a, "mem_service manager-recovery:", {
        "node": "0", "phase": "observation", "status": "pass", "fault": FAULT,
        "key": KEY, "generation": str(generation), "state": "quarantined",
        "live_refs": "1", "restarts": "2",
    })
    peer_observation = one_record(node_b, "mem_service manager-recovery:", {
        "node": "1", "phase": "observation", "status": "pass", "fault": FAULT,
        "key": KEY, "generation": str(generation), "state": "quarantined",
        "live_refs": "1", "restarts": "2",
    })
    backing_bytes = int(home_observation["backing_allocated_bytes"])
    address_bytes = int(home_observation["address_reserved_bytes"])
    require(backing_bytes > 0 and address_bytes > 0 and
            len(home_observation) == 11 and len(peer_observation) == 9,
            "isolated physical obligation was not reported")

    home_baseline, home_isolated = kernel_snapshots(node_a)
    peer_baseline, peer_isolated = kernel_snapshots(node_b)
    require(not home_baseline and not peer_baseline and not peer_isolated,
            "unexpected baseline or peer kernel resources")
    exports = [size for kind, size in home_isolated.values() if kind == "export"]
    imports = [size for kind, size in home_isolated.values() if kind == "import"]
    require(len(exports) == 1 and not imports and sum(exports) == address_bytes,
            "manager recovery did not preserve the exact home export obligation")

    return {
        "status": "pass",
        "scope": "actual-manager-sigkill-repeat-restart-isolation",
        "fault": FAULT,
        "generation": generation,
        "manager_restarts": 2,
        "manager_exit_code": 137,
        "provider_state": "isolated",
        "managed_recovery_required": True,
        "backing_allocated_bytes": backing_bytes,
        "address_reserved_bytes": address_bytes,
        "node_a_sha256": hashlib.sha256(node_a.encode()).hexdigest(),
        "node_b_sha256": hashlib.sha256(node_b.encode()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", type=Path, required=True)
    parser.add_argument("--node-b", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(args.node_a.read_text(), args.node_b.read_text())
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, KeyError, ValueError, IndexError) as error:
        parser.exit(1, f"managed manager crash validation failed: {error}\n")
    print("managed_manager_recovery=pass fault=%s generation=%d restarts=2 "
          "provider_state=isolated" % (FAULT, result["generation"]))


if __name__ == "__main__":
    main()
