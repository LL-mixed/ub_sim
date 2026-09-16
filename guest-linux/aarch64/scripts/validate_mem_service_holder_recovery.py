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
        r"old_cpu_access=rejected signal_exit=(135|139)",
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
    require_pattern(qemu_a, r"GSVA_COH: home retire route finalized",
                    "home route finalization is missing")

    return {
        "status": "pass",
        "scope": "replacement-holder-physical-revoke-and-home-retire",
        "old_cpu_signal_exit": int(signal),
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
    print("managed_holder_recovery=pass physical_revoke=1 home_retire=1")


if __name__ == "__main__":
    main()
