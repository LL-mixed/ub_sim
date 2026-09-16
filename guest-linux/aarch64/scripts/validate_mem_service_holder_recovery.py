#!/usr/bin/env python3
"""Validate replacement-holder fencing and home retirement from existing logs."""

import argparse
import hashlib
import json
import re
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def require_pattern(text: str, pattern: str, message: str) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE)
    require(match is not None, message)
    return match


def require_home_retire(node_a: str, qemu_a: str) -> dict:
    retired = re.findall(
        r"OBMM: GSVA segment retired: segment_id=(0x[0-9a-fA-F]+) "
        r"epoch=([1-9][0-9]*) status=COMMITTED",
        node_a,
    )
    require(len(retired) == 1,
            "home guest retirement marker is missing or ambiguous")
    segment_text, epoch_text = retired[0]
    segment = int(segment_text, 16)
    epoch = int(epoch_text)

    release = require_pattern(
        node_a,
        rf"^obmm-worker generation=([1-9][0-9]*) phase=release-intent "
        rf"segment={segment} export=([1-9][0-9]*)$",
        "home worker release intent does not match the retired segment",
    )
    generation = int(release.group(1))
    export_mem_id = int(release.group(2))
    for phase, export_id in (("unexported", export_mem_id),
                             ("retired", 0),
                             ("reclaimed", 0)):
        require_pattern(
            node_a,
            rf"^obmm-worker generation={generation} phase={phase} "
            rf"segment={segment} export={export_id}$",
            f"home worker {phase} phase does not match the retired segment",
        )

    updates = {
        (int(operation), int(record_segment, 16), int(record_epoch),
         int(record_export), int(error))
        for operation, record_segment, record_epoch, record_export, error
        in re.findall(
            r"^GSVA_HOME: operation=([0-9]+) "
            r"segment=(0x[0-9a-fA-F]+) epoch=([0-9]+) "
            r"export=([0-9]+) error=(-?[0-9]+)$",
            qemu_a,
            re.MULTILINE,
        )
    }
    identity = (segment, epoch, export_mem_id)
    require((1, *identity, 0) in updates,
            "home authoritative binding does not match the retired segment")
    require((2, *identity, 0) in updates,
            "home authoritative revoke does not match the retired segment")
    return {
        "segment_id": segment_text.lower(),
        "epoch": epoch,
        "export_mem_id": export_mem_id,
        "worker_generation": generation,
    }


def validate(node_a: str, node_b: str, qemu_a: str, qemu_b: str) -> dict:
    for text, role in ((node_a, "home"), (node_b, "replacement")):
        require("mem_service object-share: status=fail" not in text,
                f"{role} guest reported failure")
        require(text.count("peer_canary=ok peers=1") == 1,
                f"{role} endpoint peer canary missing or duplicated")

    require_pattern(node_a, r"mem_service recover-allocation: status=ok",
                    "home recovery was not accepted")
    require_pattern(node_a, r"^state=retiring$",
                    "home recovery did not enter RETIRING")
    require_pattern(
        node_a,
        r"action=wait_state key=holder-recovery-object status=ok state=retired",
        "home worker did not complete physical retirement",
    )
    require_pattern(
        node_a,
        r"mem_service holder-recovery: node=0 status=recovered .*state=retired",
        "home recovery completion is missing",
    )
    require_pattern(
        node_a,
        r"MANAGED_PTO_HOLDER_RECOVERY node=0 state=peer-ready generation=[1-9][0-9]*",
        "home PTO peer did not establish the pre-revoke endpoint",
    )
    require_pattern(
        node_a,
        r"MANAGED_PTO_HOLDER_RECOVERY node=0 state=peer-retired generation=[1-9][0-9]*",
        "home PTO peer did not observe retirement",
    )
    require_pattern(
        node_a,
        r"MANAGED_PTO_RESULT node=0 status=pass .*holder_recovery_probe=1 role=peer",
        "home PTO peer did not close successfully",
    )

    require_pattern(node_b, r"action=map key=holder-recovery-object status=ok",
                    "old holder mapping was not established")
    require_pattern(
        node_b,
        r"action=publish_data key=holder-recovery-object status=ok",
        "old holder payload was not published",
    )
    require(node_b.count("mem_service provider-register: status=ok") >= 2,
            "replacement provider registration is missing")
    require_pattern(node_b, r"expired_count=[1-9][0-9]*",
                    "old provider lease expiry is missing")
    require_pattern(node_b, r"^replaced=0$",
                    "replacement was not registered after old-provider removal")
    require_pattern(
        node_b,
        r"obmm-holder-fence: status=fenced key=holder-recovery-object",
        "holder fencing receipt is missing",
    )
    require_pattern(
        node_b,
        r"obmm-holder-fence: status=complete node=guest-node-1 .*allocations=1",
        "holder fencing did not complete exactly one allocation",
    )
    prepared = require_pattern(
        node_b,
        r"MANAGED_PTO_HOLDER_RECOVERY node=1 state=prepared "
        r"op_id=([1-9][0-9]*) generation=([1-9][0-9]*) "
        r"mapping_id=([1-9][0-9]*) holder_node=guest-node-1 "
        r"holder_incarnation=([1-9][0-9]*) retained_leases=3",
        "old holder PTO dispatch was not prepared before fencing",
    )
    rejected = require_pattern(
        node_b,
        r"MANAGED_PTO_HOLDER_RECOVERY node=1 state=rejected "
        r"op_id=([1-9][0-9]*) completion_op=([1-9][0-9]*) status=3 "
        r"error=(pto_ub_gm_(?:access_denied|bad_memref)) pending_op=0 "
        r"cleanup=process-exit",
        "old holder PTO dispatch was not rejected after forced revoke",
    )
    require(prepared.group(1) == rejected.group(1) == rejected.group(2),
            "old holder PTO completion identity does not match its prepared dispatch")
    require(node_b.index(prepared.group(0)) <
            node_b.index("obmm-holder-fence: status=fenced") <
            node_b.index(rejected.group(0)),
            "old holder PTO dispatch was not fenced between prepare and rejection")
    require_pattern(
        node_b,
        r"MANAGED_PTO_RESULT node=1 status=pass .*holder_recovery_probe=1 "
        r"role=stale-holder old_pto_access=rejected cleanup=process-exit",
        "old holder PTO probe did not report a bounded successful result",
    )
    require_pattern(
        node_b,
        r"action=wait_state key=holder-recovery-object status=ok state=retired",
        "old mapping process did not observe retirement",
    )
    require("action=read key=holder-recovery-object status=ok" not in node_b,
            "old CPU mapping remained readable")
    signal = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 status=fenced "
        r"old_cpu_access=rejected old_pto_access=rejected signal_exit=(135|139)",
        "old CPU mapping was not rejected after forced revoke",
    ).group(1)

    require_pattern(qemu_b, r"GSVA_TLB: flush reason=remote_retire_quarantine",
                    "holder TLB quarantine is missing")
    require_pattern(qemu_b, r"GSVA_TLB: flush reason=remote_retire_drained",
                    "holder TLB drain is missing")
    require_pattern(qemu_b, r"GSVA_UNMAP: .*tombstone=yes",
                    "holder route tombstone is missing")
    require_pattern(qemu_b, r"GSVA_RECOVERY: local holder revoked",
                    "local holder physical revoke is missing")
    require_pattern(qemu_a, r"OBMM_COH_FENCE req_id=",
                    "home QEMU did not receive the holder writeback fence request")
    require_pattern(qemu_b, r"OBMM_COH_FENCE_ACK.*status=0",
                    "holder QEMU did not receive a successful fence acknowledgement")
    home_retire = require_home_retire(node_a, qemu_a)

    return {
        "status": "pass",
        "scope": "replacement-holder-cpu-pto-revoke-and-home-retire",
        "old_cpu_signal_exit": int(signal),
        "old_pto_error": rejected.group(3),
        "old_pto_operation_id": int(rejected.group(1)),
        "old_pto_mapping_id": int(prepared.group(3)),
        "home_retire": home_retire,
        "node_a_sha256": hashlib.sha256(node_a.encode()).hexdigest(),
        "node_b_sha256": hashlib.sha256(node_b.encode()).hexdigest(),
        "qemu_a_sha256": hashlib.sha256(qemu_a.encode()).hexdigest(),
        "qemu_b_sha256": hashlib.sha256(qemu_b.encode()).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("node-a", "node-b", "qemu-a", "qemu-b", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = validate(
            args.node_a.read_text(),
            args.node_b.read_text(),
            args.qemu_a.read_text(),
            args.qemu_b.read_text(),
        )
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"managed holder recovery validation failed: {error}\n")
    print("managed_holder_recovery=pass physical_revoke=1 old_pto_rejected=1 home_retire=1")


if __name__ == "__main__":
    main()
