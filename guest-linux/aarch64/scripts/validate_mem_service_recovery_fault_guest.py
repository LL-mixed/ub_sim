#!/usr/bin/env python3
"""Validate actual reserve-intent worker crash isolation in two guest logs."""
import argparse
import hashlib
import json
from pathlib import Path

from validate_mem_service_mapping_resources import validate_kernel_regions
from validate_mem_service_reference_guest import records, require


FAULTS = {
    "reserve_intent_crash": ("sync-exit", "complete"),
    "torn_ledger_crash": ("torn-exit", "torn"),
}


def session_rows(text, node):
    rows = [row for row in records(text, "mem_service object-session:")
            if row.get("session") == f"recovery-fault-observer-{node}"]
    require(len(rows) == 4, "missing recovery observer operations or completion")
    expected = (("wait_state", "quarantined"),
                ("inspect", "quarantined"),
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
            expected_stats = {
                "live_objects": "0",
                "backing_allocated_bytes": "0",
                "address_reserved_bytes": "0",
                "live_refs": "0",
                "in_flight": "1",
                "quarantined_objects": "1",
                "quarantined_bytes": "0",
                "import_mappings": "0",
            }
            require(all(row.get(name) == value for name, value in expected_stats.items()),
                    "ambiguous allocation accounting differs")
    endings = [row for row in rows if row.get("result")]
    require(len(endings) == 1, "recovery observer did not complete exactly once")
    require(endings == [{"session": f"recovery-fault-observer-{node}",
                         "result": "ok", "ops": "3", "elapsed_ms": endings[0]["elapsed_ms"]}],
            "recovery observer did not complete exactly once")
    require(int(endings[0]["elapsed_ms"]) >= 0, "invalid observer elapsed time")
    return generation


def one_record(text, prefix, required):
    rows = records(text, prefix)
    matches = [row for row in rows
               if all(row.get(name) == value for name, value in required.items())]
    require(len(matches) == 1, f"expected one matching {prefix} record")
    return matches[0]


def validate(node_a, node_b, fault):
    require(fault in FAULTS, "unsupported recovery fault")
    mode, integrity = FAULTS[fault]
    key = f"recovery-fault-{fault}"
    generations = []
    for node, text in enumerate((node_a, node_b)):
        require("mem_service object-share: status=fail" not in text,
                "guest reported object-share failure")
        require(f"mem_service object-share: status=ok node={node} "
                f"role={'provider' if node == 0 else 'consumer'} backend=obmm" in text,
                "missing guest completion")
        generations.append(session_rows(text, node))
        observation = one_record(text, "mem_service recovery-fault:", {
            "node": str(node), "phase": "observation", "status": "pass",
            "fault": fault, "mode": mode, "ledger_integrity": integrity,
            "key": key,
        })
        require(len(observation) == 7, "unexpected recovery observation fields")
        require(text.count("data_plane_ready=0") >= 2 and
                "provider_active_count=1" in text and
                "provider_directory_ready=0" in text,
                "provider loss did not close data admission")
        validate_kernel_regions(text)

    require(generations[0] == generations[1],
            "guests observed different quarantined generations")
    triggers = [row for row in records(node_a, "obmm-worker")
                if row.get("fault-injection") == "triggered"]
    require(len(triggers) == 1 and triggers[0].get("phase") == "reserve-intent" and
            triggers[0].get("mode") == mode and int(triggers[0]["ledger_bytes"]) > 0,
            "missing deterministic worker crash receipt")
    quarantine = one_record(node_a, "obmm-worker-quarantine:", {
        "status": "complete", "node": "guest-node-0", "provider_state": "isolated",
        "ledger_integrity": integrity, "scope": "provider-control-plane",
        "backing_reconciled": "0", "resource_reconciliation_required": "1",
    })
    require(int(quarantine["incarnation"]) > 0 and len(quarantine) == 8,
            "invalid quarantine identity or extra fields")
    worker_exit = one_record(node_a, "mem_service recovery-fault:", {
        "node": "0", "phase": "worker-exit", "status": "expected",
        "fault": fault, "mode": mode, "exit_code": "86", "key": key,
        "generation": str(generations[0]),
    })
    require(len(worker_exit) == 8, "unexpected worker exit fields")
    require(len(records(node_a, "mem_service recovery-fault:")) == 2 and
            len(records(node_b, "mem_service recovery-fault:")) == 1,
            "missing or extra recovery fault lifecycle records")
    require("fault-injection=triggered" not in node_b and
            "obmm-worker-quarantine:" not in node_b and
            "phase=worker-exit" not in node_b,
            "fault was injected outside the home worker")
    return {
        "status": "pass",
        "scope": "actual-reserve-intent-worker-crash-isolation",
        "fault": fault,
        "mode": mode,
        "ledger_integrity": integrity,
        "generation": generations[0],
        "worker_exit_code": 86,
        "provider_state": "isolated",
        "resource_reconciliation_required": True,
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
    print("managed_recovery_fault=pass fault=%s generation=%d provider_state=isolated" %
          (args.fault, result["generation"]))


if __name__ == "__main__":
    main()
