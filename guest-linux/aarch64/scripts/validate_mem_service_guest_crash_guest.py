#!/usr/bin/env python3
"""Validate one lost and restarted Memory Service guest in captured logs."""
import argparse
import hashlib
import json
from pathlib import Path

from validate_mem_service_reference_guest import records, require


FAULT = "guest_crash"
KEY = "recovery-fault-guest-crash"


def one_record(text, prefix, required):
    rows = records(text, prefix)
    matches = [row for row in rows
               if all(row.get(name) == value for name, value in required.items())]
    require(len(matches) == 1, f"expected one matching {prefix} record")
    return matches[0]


def managed_mapping_records(text, prefix):
    """Return structured map records without consuming QEMU diagnostics."""
    structured = "\n".join(
        line for line in text.splitlines()
        if line.startswith(f"{prefix} map_id=")
    )
    return records(structured, prefix)


def kernel_snapshots(text):
    snapshots = []
    phase = None
    regions = {}
    for line in text.splitlines():
        if line.startswith("mem_service kernel-regions:"):
            require(phase is None, "overlapping kernel census")
            phase = line.removeprefix("mem_service kernel-regions: phase=")
            require(phase in ("baseline", "isolated"),
                    "unexpected kernel census phase")
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
            ["baseline", "isolated"], "missing guest recovery kernel census")
    return snapshots[0][1], snapshots[1][1]


def validate(node_a, node_b_before, node_b_after, qemu_b_before, host_log):
    require("mem_service object-share: status=fail" not in node_a + node_b_after,
            "surviving or replacement guest reported object-share failure")
    require("mem_service object-share: status=ok node=0 role=provider backend=obmm"
            in node_a, "missing surviving guest completion")
    require("mem_service object-share: status=ok node=1 role=consumer backend=obmm"
            in node_b_after, "missing replacement guest completion")
    require("mem_service object-share: status=ok" not in node_b_before,
            "lost guest completed before being killed")
    require("fault-injection=triggered" not in
            node_a + node_b_before + node_b_after,
            "guest crash used worker ledger fault injection")

    active = one_record(node_a, "mem_service guest-recovery:", {
        "node": "0", "phase": "active", "status": "ready", "fault": FAULT,
        "key": KEY, "old_incarnation": "7", "live_refs": "1",
        "import_mappings": "1",
    })
    generation = int(active["generation"])
    require(generation > 0 and len(active) == 9,
            "invalid active allocation receipt")
    crash_ready = one_record(node_b_before, "mem_service guest-recovery:", {
        "node": "1", "phase": "crash-ready", "status": "ready",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "incarnation": "7", "live_refs": "1", "import_mappings": "1",
        "mapping": "active",
    })
    address = int(crash_ready["address"])
    terminated_kernel_instance = crash_ready.get("kernel_instance", "")
    require(len(crash_ready) == 12 and address > 0 and
            len(terminated_kernel_instance) == 32 and
            all(ch in "0123456789abcdef"
                for ch in terminated_kernel_instance) and
            int(terminated_kernel_instance, 16) != 0,
            "invalid guest crash barrier")
    matching_routes = [row for row in
                       managed_mapping_records(qemu_b_before, "GSVA_MAP:")
                       if int(row.get("home_va", "0"), 0) == address]
    require(len(matching_routes) == 1 and
            int(matching_routes[0].get("size", "0"), 0) == 2097152 and
            matching_routes[0].get("source") == "2" and
            matching_routes[0].get("profile") == "1",
            "lost guest QEMU did not establish the exact managed mapping")
    map_id = matching_routes[0].get("map_id")
    require(not any(row.get("map_id") == map_id
                    for row in managed_mapping_records(
                        qemu_b_before, "GSVA_UNMAP:")),
            "lost guest mapping was removed before the crash")

    guest_kill = one_record(host_log, "mem_service guest-recovery-host:", {
        "phase": "guest-kill", "node": "1", "signal": "9",
        "terminated_kernel_instance": terminated_kernel_instance,
    })
    require(len(guest_kill) == 5 and int(guest_kill["pid"]) > 0,
            "unexpected guest kill receipt")
    guest_exit = one_record(host_log, "mem_service guest-recovery-host:", {
        "phase": "guest-exit", "node": "1", "status": "expected",
        "signal": "9", "exit_code": "137",
    })
    require(len(guest_exit) == 5, "unexpected guest exit receipt")
    guest_restart = one_record(host_log, "mem_service guest-recovery-host:", {
        "phase": "guest-restart", "node": "1", "old_incarnation": "7",
        "new_incarnation": "8",
    })
    require(len(guest_restart) == 4, "unexpected guest restart receipt")

    down = one_record(node_a, "mem_service guest-recovery:", {
        "node": "0", "phase": "guest-down", "status": "isolated",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "old_incarnation": "7", "state": "quarantined", "live_refs": "1",
        "import_mappings": "1", "expired_count": "1",
        "managed_recovery_required": "1", "data_plane_ready": "0",
        "identity_preserved": "1",
    })
    require(len(down) == 14, "lost guest was not isolated exactly")
    replacement = one_record(node_a, "mem_service guest-recovery:", {
        "node": "0", "phase": "replacement", "status": "isolated",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "old_incarnation": "7", "new_incarnation": "8",
        "state": "quarantined", "provider_active_count": "2",
        "provider_directory_ready": "1", "data_plane_ready": "0",
        "identity_preserved": "1",
    })
    require(len(replacement) == 13, "replacement identity did not stay isolated")
    peer = one_record(node_b_after, "mem_service guest-recovery:", {
        "node": "1", "phase": "replacement", "status": "isolated",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "incarnation": "8", "state": "quarantined",
        "managed_recovery_required": "1", "data_plane_ready": "0",
    })
    require(len(peer) == 10, "replacement guest observed unsafe state")

    deferred = one_record(node_b_after, "mem_service guest-recovery:", {
        "node": "1", "phase": "replacement-worker", "status": "deferred",
        "incarnation": "8", "reason": "holder-reconciliation-required",
    })
    require(len(deferred) == 5,
            "replacement worker started before holder reconciliation")
    fenced = one_record(node_b_after, "mem_service guest-recovery:", {
        "node": "1", "phase": "holder-fenced", "status": "complete",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "old_incarnation": "7", "new_incarnation": "8",
        "old_kernel_instance": terminated_kernel_instance,
        "route_state": "absent-after-kernel-replacement",
        "receipt": "complete",
    })
    replacement_kernel_instance = fenced.get("new_kernel_instance", "")
    require(len(fenced) == 12 and len(replacement_kernel_instance) == 32 and
            replacement_kernel_instance != terminated_kernel_instance and
            all(ch in "0123456789abcdef"
                for ch in replacement_kernel_instance) and
            int(replacement_kernel_instance, 16) != 0,
            "terminated holder lacked replacement-kernel absence proof")
    require("obmm-terminated-holder-fence: status=fenced "
            f"key={KEY} generation={generation} old_incarnation=7 "
            "current_incarnation=8 "
            "route_state=absent-after-kernel-replacement" in node_b_after,
            "provider did not bind receipt to route absence")
    require("obmm-terminated-holder-fence: status=complete "
            "node=guest-node-1 old_incarnation=7 current_incarnation=8 "
            "allocations=1 inventory_records=0" in node_b_after,
            "provider did not prove an empty stable replacement inventory")

    reconciled = one_record(node_a, "mem_service guest-recovery:", {
        "node": "0", "phase": "reconciled", "status": "retired",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "old_incarnation": "7", "new_incarnation": "8",
        "holder_receipt": "complete", "home_reclaim": "complete",
    })
    require(len(reconciled) == 10,
            "surviving home did not reclaim after the holder receipt")

    rejoin_write = one_record(node_b_after, "mem_service guest-recovery:", {
        "node": "1", "phase": "rejoin-write", "status": "pass",
        "fault": FAULT, "key": "guest-recovery-rejoin-object",
        "old_incarnation": "7", "new_incarnation": "8", "seed": "91",
    })
    rejoin_generation = int(rejoin_write["generation"])
    require(len(rejoin_write) == 9 and rejoin_generation > generation,
            "replacement worker did not publish a new generation")
    rejoin_read = one_record(node_a, "mem_service guest-recovery:", {
        "node": "0", "phase": "rejoin-read", "status": "pass",
        "fault": FAULT, "key": "guest-recovery-rejoin-object",
        "generation": str(rejoin_generation), "old_incarnation": "7",
        "new_incarnation": "8", "seed": "91",
    })
    require(len(rejoin_read) == 9,
            "surviving guest did not read the replacement payload")

    observed = one_record(node_a, "mem_service guest-recovery:", {
        "node": "0", "phase": "observation", "status": "pass",
        "fault": FAULT, "key": KEY, "generation": str(generation),
        "old_incarnation": "7", "new_incarnation": "8",
        "state": "retired", "live_refs": "0", "import_mappings": "0",
        "backing_allocated_bytes": "0", "address_reserved_bytes": "0",
        "quarantined_bytes": "0", "managed_recovery_required": "0",
        "data_plane_ready": "0", "rejoin": "pass",
    })
    require(len(observed) == 17,
            "guest-loss recovery did not return all resources to baseline")

    home_baseline, home_isolated = kernel_snapshots(node_a)
    peer_baseline, peer_isolated = kernel_snapshots(node_b_after)
    require(not home_baseline and not home_isolated and
            not peer_baseline and not peer_isolated,
            "guest-loss recovery left kernel export or import resources")

    return {
        "status": "pass",
        "scope": "actual-guest-sigkill-reconcile-rejoin",
        "fault": FAULT,
        "generation": generation,
        "guest_exit_code": 137,
        "old_incarnation": 7,
        "new_incarnation": 8,
        "provider_state": "rejoined",
        "managed_recovery_required": False,
        "live_refs": 0,
        "import_mappings": 0,
        "quarantined_bytes": 0,
        "terminated_kernel_instance": terminated_kernel_instance,
        "replacement_kernel_instance": replacement_kernel_instance,
        "rejoin_generation": rejoin_generation,
        "node_a_sha256": hashlib.sha256(node_a.encode()).hexdigest(),
        "node_b_before_sha256": hashlib.sha256(node_b_before.encode()).hexdigest(),
        "node_b_after_sha256": hashlib.sha256(node_b_after.encode()).hexdigest(),
        "qemu_b_before_sha256": hashlib.sha256(qemu_b_before.encode()).hexdigest(),
        "host_log_sha256": hashlib.sha256(host_log.encode()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", type=Path, required=True)
    parser.add_argument("--node-b-before", type=Path, required=True)
    parser.add_argument("--node-b-after", type=Path, required=True)
    parser.add_argument("--qemu-b-before", type=Path, required=True)
    parser.add_argument("--host-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(
            args.node_a.read_text(), args.node_b_before.read_text(),
            args.node_b_after.read_text(), args.qemu_b_before.read_text(),
            args.host_log.read_text())
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, KeyError, ValueError, IndexError) as error:
        parser.exit(1, f"managed guest crash validation failed: {error}\n")
    print("managed_guest_recovery=pass fault=%s generation=%d "
          "old_incarnation=7 new_incarnation=8 provider_state=rejoined "
          "holder_fence=1 home_reclaim=1 rejoin=1" %
          (FAULT, result["generation"]))


if __name__ == "__main__":
    main()
