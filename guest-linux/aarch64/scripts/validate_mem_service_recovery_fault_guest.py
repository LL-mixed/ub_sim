#!/usr/bin/env python3
"""Validate actual OBMM worker transaction crash recovery in two guest logs."""
import argparse
import hashlib
import json
from pathlib import Path

from validate_mem_service_mapping_resources import validate_kernel_regions
from validate_mem_service_reference_guest import records, require


FAULTS = {
    "reserve_intent_crash": {
        "phase": "reserve-intent", "mode": "sync-exit",
        "integrity": "complete", "recovery": "quarantined",
        "final_state": "quarantined", "resume_state": None,
    },
    "torn_ledger_crash": {
        "phase": "reserve-intent", "mode": "torn-exit",
        "integrity": "torn", "recovery": "quarantined",
        "final_state": "quarantined", "resume_state": None,
    },
    "address_reserved_crash": {
        "phase": "reserved", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "active",
    },
    "backing_exported_crash": {
        "phase": "exported", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "active",
    },
    "published_crash": {
        "phase": "published", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "active",
    },
    "release_intent_crash": {
        "phase": "release-intent", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "retired",
    },
    "unexported_crash": {
        "phase": "unexported", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "retired",
    },
    "retired_crash": {
        "phase": "retired", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "retired",
    },
    "reclaimed_crash": {
        "phase": "reclaimed", "mode": "sync-exit",
        "integrity": "complete", "recovery": "resumed",
        "final_state": "retired", "resume_state": "retired",
    },
}


def session_rows(text, node, spec):
    rows = [row for row in records(text, "mem_service object-session:")
            if row.get("session") == f"recovery-fault-observer-{node}"]
    require(len(rows) == 4, "missing recovery observer operations or completion")
    expected = (("wait_state", spec["final_state"]),
                ("inspect", spec["final_state"]),
                ("stats", None))
    operations = [row for row in rows if "op" in row]
    require(len(operations) == len(expected), "wrong recovery observer operation count")
    generation = None
    for index, (row, (action, state)) in enumerate(zip(operations, expected), 1):
        require(row.get("op") == str(index) and row.get("action") == action and
                row.get("status") == "ok", "recovery observer operation failed or reordered")
        if state is not None:
            require(row.get("state") == state and int(row["generation"]) > 0,
                    "quarantined identity was not observed")
            generation = generation or int(row["generation"])
            require(int(row["generation"]) == generation,
                    "quarantined generation changed between observations")
        else:
            expected_stats = dict(
                live_objects="0", backing_allocated_bytes="0",
                address_reserved_bytes="0", live_refs="0",
                in_flight="1" if spec["recovery"] == "quarantined" else "0",
                quarantined_objects=(
                    "1" if spec["recovery"] == "quarantined" else "0"
                ),
                quarantined_bytes="0", import_mappings="0",
            )
            require(all(row.get(name) == value for name, value in expected_stats.items()),
                    "ambiguous allocation accounting differs")
    endings = [row for row in rows if row.get("result")]
    require(len(endings) == 1, "recovery observer did not complete exactly once")
    require(endings == [{"session": f"recovery-fault-observer-{node}",
                         "result": "ok", "ops": "3", "elapsed_ms": endings[0]["elapsed_ms"]}],
            "recovery observer did not complete exactly once")
    require(int(endings[0]["elapsed_ms"]) >= 0, "invalid observer elapsed time")
    return generation


def validate_active_resume_controller(text, spec):
    rows = [row for row in records(text, "mem_service object-session:")
            if row.get("session") == "recovery-fault-resume-controller"]
    if spec["resume_state"] != "active":
        require(not rows, "unexpected active resume controller")
        return
    operations = [row for row in rows if "op" in row]
    require(len(rows) == 5 and len(operations) == 4,
            "missing active resume lifecycle")
    expected = (("wait_state", "active"), ("inspect", "active"),
                ("retire", None), ("wait_state", "retired"))
    for index, (row, (action, state)) in enumerate(zip(operations, expected), 1):
        require(row.get("op") == str(index) and row.get("action") == action and
                row.get("status") == "ok", "active resume lifecycle failed")
        if state:
            require(row.get("state") == state,
                    "active resume lifecycle state differs")
    endings = [row for row in rows if row.get("result")]
    require(len(endings) == 1 and endings[0].get("result") == "ok" and
            endings[0].get("ops") == "4",
            "active resume controller did not complete")


def one_record(text, prefix, required):
    rows = records(text, prefix)
    matches = [row for row in rows
               if all(row.get(name) == value for name, value in required.items())]
    require(len(matches) == 1, f"expected one matching {prefix} record")
    return matches[0]


def validate(node_a, node_b, fault):
    require(fault in FAULTS, "unsupported recovery fault")
    spec = FAULTS[fault]
    mode, integrity = spec["mode"], spec["integrity"]
    key = f"recovery-fault-{fault}"
    generations = []
    for node, text in enumerate((node_a, node_b)):
        require("mem_service object-share: status=fail" not in text,
                "guest reported object-share failure")
        require(f"mem_service object-share: status=ok node={node} "
                f"role={'provider' if node == 0 else 'consumer'} backend=obmm" in text,
                "missing guest completion")
        generations.append(session_rows(text, node, spec))
        observation = one_record(text, "mem_service recovery-fault:", {
            "node": str(node), "phase": "observation", "status": "pass",
            "fault": fault, "injection_phase": spec["phase"], "mode": mode,
            "ledger_integrity": integrity, "recovery": spec["recovery"],
            "final_state": spec["final_state"], "key": key,
        })
        require(len(observation) == 10, "unexpected recovery observation fields")
        expected_active = "1" if spec["recovery"] == "quarantined" else "2"
        expected_ready = "0" if spec["recovery"] == "quarantined" else "1"
        require(text.count("data_plane_ready=0") >= 2 and
                f"provider_active_count={expected_active}" in text and
                f"provider_directory_ready={expected_ready}" in text,
                "provider directory recovery state differs")
        validate_kernel_regions(text)

    require(generations[0] == generations[1],
            "guests observed different quarantined generations")
    triggers = records(node_a, "obmm-worker fault-injection=triggered")
    require(len(triggers) == 1 and triggers[0].get("phase") == spec["phase"] and
            triggers[0].get("mode") == mode and int(triggers[0]["ledger_bytes"]) > 0,
            "missing deterministic worker crash receipt")
    quarantine = records(node_a, "obmm-worker-quarantine:")
    resumes = records(node_a, "obmm-worker-resume:")
    if spec["recovery"] == "quarantined":
        require(len(quarantine) == 1 and not resumes,
                "quarantine recovery evidence differs")
        required = {
            "status": "complete", "node": "guest-node-0",
            "provider_state": "isolated", "ledger_integrity": integrity,
            "scope": "provider-control-plane", "backing_reconciled": "0",
            "resource_reconciliation_required": "1",
        }
        require(all(quarantine[0].get(name) == value
                    for name, value in required.items()) and
                int(quarantine[0]["incarnation"]) > 0 and
                len(quarantine[0]) == 8,
                "invalid quarantine identity or extra fields")
    else:
        expected_reservations = 1 if spec["resume_state"] == "active" else 0
        require(not quarantine and len(resumes) == 1 and
                resumes[0].get("status") == "ready" and
                int(resumes[0].get("reservations", "-1")) ==
                expected_reservations and
                resumes[0].get("scope") ==
                "ledger-owned-segment-export-records",
                "worker resume evidence differs")
        validate_active_resume_controller(node_a, spec)
    worker_exit = one_record(node_a, "mem_service recovery-fault:", {
        "node": "0", "phase": "worker-exit", "status": "expected",
        "fault": fault, "injection_phase": spec["phase"], "mode": mode,
        "exit_code": "86", "key": key, "generation": str(generations[0]),
    })
    require(len(worker_exit) == 9, "unexpected worker exit fields")
    require(len(records(node_a, "mem_service recovery-fault:")) == 2 and
            len(records(node_b, "mem_service recovery-fault:")) == 1,
            "missing or extra recovery fault lifecycle records")
    require("fault-injection=triggered" not in node_b and
            "obmm-worker-quarantine:" not in node_b and
            "phase=worker-exit" not in node_b,
            "fault was injected outside the home worker")
    return {
        "status": "pass",
        "scope": "actual-obmm-worker-transaction-crash-recovery",
        "fault": fault,
        "injection_phase": spec["phase"],
        "mode": mode,
        "ledger_integrity": integrity,
        "recovery": spec["recovery"],
        "final_state": spec["final_state"],
        "generation": generations[0],
        "worker_exit_code": 86,
        "provider_state": (
            "isolated" if spec["recovery"] == "quarantined" else "ready"
        ),
        "resource_reconciliation_required": spec["recovery"] == "quarantined",
        "node_a_sha256": hashlib.sha256(node_a.encode()).hexdigest(),
        "node_b_sha256": hashlib.sha256(node_b.encode()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-a", type=Path, required=True)
    parser.add_argument("--node-b", type=Path, required=True)
    parser.add_argument("--fault", choices=sorted(FAULTS), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(args.node_a.read_text(), args.node_b.read_text(), args.fault)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, KeyError, ValueError, IndexError) as error:
        parser.exit(1, f"managed recovery fault validation failed: {error}\n")
    print("managed_recovery_fault=pass fault=%s generation=%d provider_state=%s" %
          (args.fault, result["generation"], result["provider_state"]))


if __name__ == "__main__":
    main()
