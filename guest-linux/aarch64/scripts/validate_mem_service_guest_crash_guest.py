#!/usr/bin/env python3
"""Validate one or more lost and restarted Memory Service guests."""
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


def validate(node_a, node_b_before, node_b_after, qemu_b_before, host_log,
             cycles=1, base_incarnation=7):
    node_b_before = ([node_b_before] if isinstance(node_b_before, str)
                     else list(node_b_before))
    qemu_b_before = ([qemu_b_before] if isinstance(qemu_b_before, str)
                     else list(qemu_b_before))
    require(1 <= cycles <= 8 and len(node_b_before) == cycles and
            len(qemu_b_before) == cycles and base_incarnation > 0,
            "guest crash cycle inputs do not match")
    peer_text = "".join(node_b_before) + node_b_after
    require("mem_service object-share: status=fail" not in node_a + peer_text,
            "surviving or replacement guest reported object-share failure")
    require("mem_service object-share: status=ok node=0 role=provider backend=obmm"
            in node_a, "missing surviving guest completion")
    require("mem_service object-share: status=ok node=1 role=consumer backend=obmm"
            in node_b_after, "missing replacement guest completion")
    require(all("mem_service object-share: status=ok" not in text
                for text in node_b_before),
            "a lost guest completed before being killed")
    require("fault-injection=triggered" not in
            node_a + peer_text,
            "guest crash used worker ledger fault injection")
    cycle_results = []
    last_generation = 0
    previous_replacement_kernel = None
    for cycle in range(1, cycles + 1):
        suffix = "" if cycle == 1 else f"-{cycle}"
        key = f"{KEY}{suffix}"
        rejoin_key = f"guest-recovery-rejoin-object{suffix}"
        old_incarnation = base_incarnation + cycle - 1
        new_incarnation = old_incarnation + 1
        seed = 90 + cycle
        cycle_text = node_b_before[cycle - 1]
        replacement_text = (node_b_before[cycle]
                            if cycle < cycles else node_b_after)

        active = one_record(node_a, "mem_service guest-recovery:", {
            "node": "0", "phase": "active", "status": "ready",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "old_incarnation": str(old_incarnation), "live_refs": "1",
            "import_mappings": "1",
        })
        generation = int(active["generation"])
        require(generation > last_generation and len(active) == 10,
                "invalid active allocation receipt")
        crash_ready = one_record(cycle_text, "mem_service guest-recovery:", {
            "node": "1", "phase": "crash-ready", "status": "ready",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "incarnation": str(old_incarnation), "live_refs": "1",
            "import_mappings": "1", "mapping": "active",
        })
        address = int(crash_ready["address"])
        terminated_kernel_instance = crash_ready.get("kernel_instance", "")
        require(len(crash_ready) == 13 and address > 0 and
                len(terminated_kernel_instance) == 32 and
                all(ch in "0123456789abcdef"
                    for ch in terminated_kernel_instance) and
                int(terminated_kernel_instance, 16) != 0,
                "invalid guest crash barrier")
        if previous_replacement_kernel is not None:
            require(terminated_kernel_instance == previous_replacement_kernel,
                    "replacement kernel identity chain is discontinuous")
        matching_routes = [
            row for row in managed_mapping_records(
                qemu_b_before[cycle - 1], "GSVA_MAP:")
            if int(row.get("home_va", "0"), 0) == address
        ]
        require(len(matching_routes) == 1 and
                int(matching_routes[0].get("size", "0"), 0) == 2097152 and
                matching_routes[0].get("source") == "2" and
                matching_routes[0].get("profile") == "1",
                "lost guest QEMU did not establish the exact managed mapping")
        map_id = matching_routes[0].get("map_id")
        require(not any(row.get("map_id") == map_id
                        for row in managed_mapping_records(
                            qemu_b_before[cycle - 1], "GSVA_UNMAP:")),
                "lost guest mapping was removed before the crash")

        guest_kill = one_record(host_log, "mem_service guest-recovery-host:", {
            "phase": "guest-kill", "cycle": str(cycle), "node": "1",
            "signal": "9",
            "terminated_kernel_instance": terminated_kernel_instance,
        })
        require(len(guest_kill) == 6 and int(guest_kill["pid"]) > 0,
                "unexpected guest kill receipt")
        guest_exit = one_record(host_log, "mem_service guest-recovery-host:", {
            "phase": "guest-exit", "cycle": str(cycle), "node": "1",
            "status": "expected", "signal": "9", "exit_code": "137",
        })
        require(len(guest_exit) == 6, "unexpected guest exit receipt")
        guest_restart = one_record(host_log, "mem_service guest-recovery-host:", {
            "phase": "guest-restart", "cycle": str(cycle), "node": "1",
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation),
        })
        require(len(guest_restart) == 5, "unexpected guest restart receipt")

        down = one_record(node_a, "mem_service guest-recovery:", {
            "node": "0", "phase": "guest-down", "status": "isolated",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "old_incarnation": str(old_incarnation), "state": "quarantined",
            "live_refs": "1", "import_mappings": "1",
            "expired_count": str(cycle), "managed_recovery_required": "1",
            "data_plane_ready": "0", "identity_preserved": "1",
        })
        require(len(down) == 15, "lost guest was not isolated exactly")
        replacement = one_record(node_a, "mem_service guest-recovery:", {
            "node": "0", "phase": "replacement", "status": "isolated",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation), "state": "quarantined",
            "provider_active_count": "2", "provider_directory_ready": "1",
            "data_plane_ready": "0", "identity_preserved": "1",
        })
        require(len(replacement) == 14,
                "replacement identity did not stay isolated")
        peer = one_record(replacement_text, "mem_service guest-recovery:", {
            "node": "1", "phase": "replacement", "status": "isolated",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "incarnation": str(new_incarnation), "state": "quarantined",
            "managed_recovery_required": "1", "data_plane_ready": "0",
        })
        require(len(peer) == 11, "replacement guest observed unsafe state")
        deferred = one_record(replacement_text, "mem_service guest-recovery:", {
            "node": "1", "phase": "replacement-worker", "status": "deferred",
            "cycle": str(cycle), "incarnation": str(new_incarnation),
            "reason": "holder-reconciliation-required",
        })
        require(len(deferred) == 6,
                "replacement worker started before holder reconciliation")
        fenced = one_record(replacement_text, "mem_service guest-recovery:", {
            "node": "1", "phase": "holder-fenced", "status": "complete",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation),
            "old_kernel_instance": terminated_kernel_instance,
            "route_state": "absent-after-kernel-replacement",
            "receipt": "complete",
        })
        replacement_kernel_instance = fenced.get("new_kernel_instance", "")
        require(len(fenced) == 13 and
                len(replacement_kernel_instance) == 32 and
                replacement_kernel_instance != terminated_kernel_instance and
                all(ch in "0123456789abcdef"
                    for ch in replacement_kernel_instance) and
                int(replacement_kernel_instance, 16) != 0,
                "terminated holder lacked replacement-kernel absence proof")
        require("obmm-terminated-holder-fence: status=fenced "
                f"key={key} generation={generation} "
                f"old_incarnation={old_incarnation} "
                f"current_incarnation={new_incarnation} "
                "route_state=absent-after-kernel-replacement" in replacement_text,
                "provider did not bind receipt to route absence")
        require("obmm-terminated-holder-fence: status=complete "
                f"node=guest-node-1 old_incarnation={old_incarnation} "
                f"current_incarnation={new_incarnation} allocations=1 "
                "inventory_records=0" in replacement_text,
                "provider did not prove an empty stable replacement inventory")

        reconciled = one_record(node_a, "mem_service guest-recovery:", {
            "node": "0", "phase": "reconciled", "status": "retired",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation),
            "holder_receipt": "complete", "home_reclaim": "complete",
        })
        require(len(reconciled) == 11,
                "surviving home did not reclaim after the holder receipt")
        rejoin_write = one_record(replacement_text,
                                  "mem_service guest-recovery:", {
            "node": "1", "phase": "rejoin-write", "status": "pass",
            "fault": FAULT, "cycle": str(cycle), "key": rejoin_key,
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation), "seed": str(seed),
        })
        rejoin_generation = int(rejoin_write["generation"])
        require(len(rejoin_write) == 10 and rejoin_generation > generation,
                "replacement worker did not publish a new generation")
        rejoin_read = one_record(node_a, "mem_service guest-recovery:", {
            "node": "0", "phase": "rejoin-read", "status": "pass",
            "fault": FAULT, "cycle": str(cycle), "key": rejoin_key,
            "generation": str(rejoin_generation),
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation), "seed": str(seed),
        })
        require(len(rejoin_read) == 10,
                "surviving guest did not read the replacement payload")
        observed = one_record(node_a, "mem_service guest-recovery:", {
            "node": "0", "phase": "observation", "status": "pass",
            "fault": FAULT, "cycle": str(cycle), "key": key,
            "generation": str(generation),
            "old_incarnation": str(old_incarnation),
            "new_incarnation": str(new_incarnation), "state": "retired",
            "live_refs": "0", "import_mappings": "0",
            "backing_allocated_bytes": "0", "address_reserved_bytes": "0",
            "quarantined_bytes": "0", "managed_recovery_required": "0",
            "data_plane_ready": "0", "rejoin": "pass",
        })
        require(len(observed) == 18,
                "guest-loss recovery did not return all resources to baseline")
        cycle_results.append({
            "cycle": cycle,
            "generation": generation,
            "old_incarnation": old_incarnation,
            "new_incarnation": new_incarnation,
            "terminated_kernel_instance": terminated_kernel_instance,
            "replacement_kernel_instance": replacement_kernel_instance,
            "rejoin_generation": rejoin_generation,
        })
        previous_replacement_kernel = replacement_kernel_instance
        last_generation = rejoin_generation

    home_baseline, home_isolated = kernel_snapshots(node_a)
    peer_baseline, peer_isolated = kernel_snapshots(node_b_after)
    require(not home_baseline and not home_isolated and
            not peer_baseline and not peer_isolated,
            "guest-loss recovery left kernel export or import resources")

    return {
        "status": "pass",
        "scope": "actual-guest-sigkill-reconcile-rejoin",
        "fault": FAULT,
        "cycles": cycles,
        "cycle_results": cycle_results,
        "generation": cycle_results[0]["generation"],
        "guest_exit_code": 137,
        "old_incarnation": base_incarnation,
        "new_incarnation": base_incarnation + cycles,
        "provider_state": "rejoined",
        "managed_recovery_required": False,
        "live_refs": 0,
        "import_mappings": 0,
        "quarantined_bytes": 0,
        "terminated_kernel_instance": cycle_results[0]["terminated_kernel_instance"],
        "replacement_kernel_instance": cycle_results[-1]["replacement_kernel_instance"],
        "rejoin_generation": cycle_results[-1]["rejoin_generation"],
        "node_a_sha256": hashlib.sha256(node_a.encode()).hexdigest(),
        "node_b_before_sha256": [hashlib.sha256(text.encode()).hexdigest()
                                  for text in node_b_before],
        "node_b_after_sha256": hashlib.sha256(node_b_after.encode()).hexdigest(),
        "qemu_b_before_sha256": [hashlib.sha256(text.encode()).hexdigest()
                                  for text in qemu_b_before],
        "host_log_sha256": hashlib.sha256(host_log.encode()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", type=Path, required=True)
    parser.add_argument("--node-b-before", type=Path, action="append", required=True)
    parser.add_argument("--node-b-after", type=Path, required=True)
    parser.add_argument("--qemu-b-before", type=Path, action="append", required=True)
    parser.add_argument("--host-log", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--base-incarnation", type=int, default=7)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(
            args.node_a.read_text(),
            [path.read_text() for path in args.node_b_before],
            args.node_b_after.read_text(),
            [path.read_text() for path in args.qemu_b_before],
            args.host_log.read_text(), args.cycles, args.base_incarnation)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, KeyError, ValueError, IndexError) as error:
        parser.exit(1, f"managed guest crash validation failed: {error}\n")
    print("managed_guest_recovery=pass fault=%s cycles=%d generation=%d "
          "old_incarnation=%d new_incarnation=%d provider_state=rejoined "
          "holder_fence=1 home_reclaim=1 rejoin=1" %
          (FAULT, result["cycles"], result["generation"],
           result["old_incarnation"], result["new_incarnation"]))


if __name__ == "__main__":
    main()
