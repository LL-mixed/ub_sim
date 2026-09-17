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
    home_canary = require_pattern(
        node_a,
        r"MANAGED_PTO node=0 peer_canary=pass bootstrap_generation=([1-9][0-9]*)",
        "home PTO endpoint peer canary is missing",
    )
    replacement_canary = require_pattern(
        node_b,
        r"MANAGED_PTO node=1 peer_canary=pass bootstrap_generation=([1-9][0-9]*)",
        "replacement PTO endpoint peer canary is missing",
    )
    require(node_a.count("MANAGED_PTO node=0 peer_canary=pass") == 1,
            "home PTO endpoint peer canary is duplicated")
    require(node_b.count("MANAGED_PTO node=1 peer_canary=pass") == 1,
            "replacement PTO endpoint peer canary is duplicated")
    require(home_canary.group(1) == replacement_canary.group(1),
            "PTO peers used different bootstrap discovery generations")

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

    require_pattern(
        node_b,
        r"MANAGED_PTO node=1 key=holder-recovery-object generation=[1-9][0-9]* "
        r"cpu_base=0x[0-9a-f]+ .*mapping_id=[1-9][0-9]* ",
        "old holder PTO mapping was not established",
    )
    require_pattern(
        node_b,
        r"MANAGED_OWNER node=1 state=adopted generation=[1-9][0-9]* "
        r"mapping_id=[1-9][0-9]* cpu_pins=1 holder_transferred=1",
        "old holder mapping was not adopted by one CPU/PTO owner",
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
    submit_started = require_pattern(
        node_b,
        r"MANAGED_PTO_HOLDER_RECOVERY node=1 state=submit-start "
        r"op_id=([1-9][0-9]*) generation=([1-9][0-9]*) "
        r"mapping_id=([1-9][0-9]*) retained_cpu_pins=1 retained_leases=3",
        "old holder PTO dispatch was not submitted after retirement",
    )
    observed = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=pto-prepared-observed "
        r"op_id=([1-9][0-9]*)",
        "controller did not observe the prepared PTO dispatch before fencing",
    )
    rejected_observed = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=pto-rejected-observed "
        r"status=ok op_id=([1-9][0-9]*)",
        "controller did not observe the stale PTO rejection",
    )
    cpu_probe_started = require_pattern(
        node_b,
        r"MANAGED_PTO_HOLDER_RECOVERY node=1 state=cpu-probe-start "
        r"op_id=([1-9][0-9]*) generation=([1-9][0-9]*) "
        r"mapping_id=([1-9][0-9]*) retained_cpu_pins=1 retained_leases=3",
        "old holder CPU probe did not start after PTO rejection",
    )
    cpu_probe_observed = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=cpu-probe-start-observed "
        r"status=ok op_id=([1-9][0-9]*)",
        "controller did not observe the bounded CPU probe start",
    )
    cpu_rejected = require_pattern(
        node_b,
        r"MANAGED_PTO_HOLDER_RECOVERY node=1 state=cpu-rejected "
        r"op_id=([1-9][0-9]*) generation=([1-9][0-9]*) "
        r"mapping_id=([1-9][0-9]*) signal=(7|11) code=([1-9][0-9]*) "
        r"address=0x[0-9a-f]+ retained_cpu_pins=1 retained_leases=3",
        "old holder CPU access was not rejected inside the PTO owner",
    )
    cpu_rejected_observed = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=cpu-rejected-observed "
        r"status=ok op_id=([1-9][0-9]*)",
        "controller did not observe the bounded CPU rejection",
    )
    require(prepared.group(1) == rejected.group(1) == rejected.group(2),
            "old holder PTO completion identity does not match its prepared dispatch")
    require(int(home_canary.group(1)) != int(prepared.group(4)),
            "PTO bootstrap discovery reused the fenced provider incarnation")
    require(prepared.group(1) == submit_started.group(1) == observed.group(1) ==
            rejected_observed.group(1) == cpu_probe_started.group(1) ==
            cpu_probe_observed.group(1) == cpu_rejected.group(1) ==
            cpu_rejected_observed.group(1),
            "CPU/PTO rejection does not match the observed prepared dispatch")
    require(prepared.group(3) == submit_started.group(3) ==
            cpu_probe_started.group(3) == cpu_rejected.group(3),
            "CPU rejection does not match the prepared mapping")
    require(node_b.index(observed.group(0)) <
            node_b.index("obmm-holder-fence: status=fenced") <
            node_b.index(rejected_observed.group(0)) <
            node_b.index(cpu_probe_observed.group(0)) <
            node_b.index(cpu_rejected_observed.group(0)),
            "old holder accesses were not rejected after the observed fence")
    require(node_b.index(prepared.group(0)) <
            node_b.index(submit_started.group(0)) <
            node_b.index(rejected.group(0)) <
            node_b.index(cpu_probe_started.group(0)) <
            node_b.index(cpu_rejected.group(0)),
            "old holder PTO and CPU probes did not follow the bounded post-revoke order")
    require_pattern(
        node_b,
        r"MANAGED_PTO_RESULT node=1 status=pass .*holder_recovery_probe=1 "
        r"role=stale-holder old_cpu_access=rejected old_pto_access=rejected "
        r"cpu_signal=(7|11) cleanup=process-exit",
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
        r"old_cpu_access=rejected old_pto_access=rejected old_cpu_signal=(7|11)",
        "old CPU mapping was not rejected after forced revoke",
    ).group(1)
    require(signal == cpu_rejected.group(4),
            "controller CPU signal does not match the PTO owner probe")

    rejoin_prepared = require_pattern(
        node_b,
        r"obmm-holder-rejoin: status=prepared node=guest-node-1 "
        r"old_incarnation=([1-9][0-9]*) current_incarnation=([1-9][0-9]*) "
        r"ledger_entries=1 home_obligations=0 holder_obligations=0 "
        r"archive=\S+\.holder-fenced-[0-9]+-by-[0-9]+",
        "empty holder ledger was not safely archived for rejoin",
    )
    rejoin_worker = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=rejoin-worker "
        r"status=started incarnation=([1-9][0-9]*) "
        r"state_file=/tmp/managed-share\.worker\.state",
        "replacement worker was not started on the configured state path",
    )
    rejoin_allocation = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=rejoin-allocation "
        r"status=active key=holder-recovery-rejoin-object "
        r"generation=([1-9][0-9]*) home_incarnation=([1-9][0-9]*)",
        "replacement worker did not publish a new home allocation",
    )
    stale_identity = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=rejoin-stale-identity "
        r"status=rejected old_incarnation=([1-9][0-9]*) "
        r"current_incarnation=([1-9][0-9]*) "
        r"reason=holder_provider_mismatch",
        "rejoin did not reject the old provider identity",
    )
    require_pattern(
        node_b,
        r"mem_service object-session: session=holder-recovery-rejoin-writer "
        r"peer_canary=ok peers=1",
        "replacement writer did not establish its peer mapping",
    )
    rejoin_writer_acquire = require_pattern(
        node_b,
        r"action=acquire key=holder-recovery-rejoin-object status=ok "
        r"state=active generation=([1-9][0-9]*)",
        "replacement writer did not acquire the rejoined allocation",
    )
    rejoin_writer_write = require_pattern(
        node_b,
        r"action=write key=holder-recovery-rejoin-object status=ok "
        r"offset=([0-9]+) len=([1-9][0-9]*) checksum=(0x[0-9a-f]+)",
        "replacement writer did not write the rejoined allocation",
    )
    rejoin_writer_publish = require_pattern(
        node_b,
        r"action=publish_data key=holder-recovery-rejoin-object status=ok "
        r"offset=([0-9]+) len=([1-9][0-9]*) checksum=(0x[0-9a-f]+)",
        "replacement writer did not publish the rejoined allocation",
    )
    require_pattern(
        node_b,
        r"obmm-worker work key=holder-recovery-rejoin-object "
        r"generation=[1-9][0-9]* state=allocating",
        "replacement worker did not consume the new allocation",
    )
    rejoin_write = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 phase=rejoin-write "
        r"status=pass key=holder-recovery-rejoin-object "
        r"generation=([1-9][0-9]*) home_incarnation=([1-9][0-9]*)",
        "replacement writer did not complete the rejoin flow",
    )
    rejoin_done = require_pattern(
        node_b,
        r"mem_service holder-recovery: node=1 status=rejoined "
        r"old_incarnation=([1-9][0-9]*) "
        r"current_incarnation=([1-9][0-9]*) "
        r"key=holder-recovery-rejoin-object generation=([1-9][0-9]*)",
        "replacement provider did not report complete rejoin",
    )
    require_pattern(
        node_a,
        r"mem_service object-session: session=holder-recovery-rejoin-reader "
        r"peer_canary=ok peers=1",
        "remote reader did not establish its peer mapping to the replacement",
    )
    rejoin_acquire = require_pattern(
        node_a,
        r"action=acquire key=holder-recovery-rejoin-object status=ok "
        r"state=active generation=([1-9][0-9]*)",
        "remote reader did not acquire the rejoined allocation",
    )
    rejoin_reader_read = require_pattern(
        node_a,
        r"action=read key=holder-recovery-rejoin-object status=ok "
        r"offset=([0-9]+) len=([1-9][0-9]*) checksum=(0x[0-9a-f]+)",
        "remote reader did not read the rejoined allocation",
    )
    rejoin_read = require_pattern(
        node_a,
        r"mem_service holder-recovery: node=0 phase=rejoin-read "
        r"status=pass key=holder-recovery-rejoin-object "
        r"generation=([1-9][0-9]*) holder_incarnation=([1-9][0-9]*) "
        r"home_incarnation=([1-9][0-9]*)",
        "remote reader did not complete the rejoined allocation flow",
    )
    old_incarnation = prepared.group(4)
    current_incarnation = str(int(old_incarnation) + 1)
    rejoin_generation = rejoin_allocation.group(1)
    require(rejoin_prepared.groups() == (old_incarnation, current_incarnation),
            "holder ledger archive used the wrong replacement identity")
    require(rejoin_worker.group(1) == current_incarnation,
            "replacement worker used the wrong incarnation")
    require(rejoin_allocation.group(2) == current_incarnation,
            "new allocation was not bound to the replacement incarnation")
    require(stale_identity.groups() == (old_incarnation, current_incarnation),
            "stale-identity rejection did not match the provider replacement")
    require(rejoin_writer_acquire.group(1) == rejoin_generation,
            "replacement writer acquire identity does not match the new allocation")
    require(rejoin_writer_write.groups() == rejoin_writer_publish.groups() ==
            rejoin_reader_read.groups(),
            "rejoin payload checksum does not match across write, publish, and read")
    require(rejoin_write.groups() == (rejoin_generation, current_incarnation),
            "replacement write identity does not match the new allocation")
    require(rejoin_done.groups() ==
            (old_incarnation, current_incarnation, rejoin_generation),
            "rejoin completion identity does not match the new allocation")
    require(rejoin_acquire.group(1) == rejoin_generation,
            "remote reader acquire identity does not match the new allocation")
    require(rejoin_read.group(1) == rejoin_generation and
            rejoin_read.group(3) == current_incarnation,
            "remote read identity does not match the new allocation")
    require(node_b.index(cpu_rejected_observed.group(0)) <
            node_b.index(rejoin_prepared.group(0)) <
            node_b.index(rejoin_worker.group(0)) <
            node_b.index(rejoin_allocation.group(0)) <
            node_b.index(stale_identity.group(0)) <
            node_b.index(rejoin_write.group(0)) <
            node_b.index(rejoin_done.group(0)),
            "replacement rejoin did not follow completed old-access fencing")

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
        "scope": "replacement-holder-revoke-home-retire-and-rejoin",
        "old_cpu_signal": int(signal),
        "old_pto_error": rejected.group(3),
        "old_pto_operation_id": int(rejected.group(1)),
        "old_pto_mapping_id": int(prepared.group(3)),
        "pto_bootstrap_generation": int(home_canary.group(1)),
        "home_retire": home_retire,
        "rejoin": {
            "key": "holder-recovery-rejoin-object",
            "generation": int(rejoin_generation),
            "old_incarnation": int(old_incarnation),
            "current_incarnation": int(current_incarnation),
            "reader_incarnation": int(rejoin_read.group(2)),
        },
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
    print("managed_holder_recovery=pass physical_revoke=1 old_pto_rejected=1 "
          "home_retire=1 rejoin=1")


if __name__ == "__main__":
    main()
