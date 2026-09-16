import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "guest-linux/aarch64/scripts"
SPEC = importlib.util.spec_from_file_location(
    "holder_recovery_validator",
    SCRIPTS / "validate_mem_service_holder_recovery.py",
)
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


def fixture():
    node_a = "\n".join((
        "MANAGED_PTO node=0 peer_canary=pass bootstrap_generation=9001",
        "MANAGED_PTO_HOLDER_RECOVERY node=0 state=peer-ready generation=1",
        "mem_service recover-allocation: status=ok",
        "state=retiring",
        "obmm-worker generation=1 phase=release-intent segment=14177894576915742721 export=1",
        "obmm-worker generation=1 phase=unexported segment=14177894576915742721 export=1",
        "OBMM: GSVA segment retired: segment_id=0xc4c2000000000001 epoch=1 status=COMMITTED",
        "obmm-worker generation=1 phase=retired segment=14177894576915742721 export=0",
        "obmm-worker generation=1 phase=reclaimed segment=14177894576915742721 export=0",
        "mem_service object-session: action=wait_state key=holder-recovery-object status=ok state=retired",
        "MANAGED_PTO_HOLDER_RECOVERY node=0 state=peer-retired generation=1",
        "MANAGED_PTO_RESULT node=0 status=pass key=holder-recovery-object generation=1 holder_recovery_probe=1 role=peer old_cpu_access=not-applicable old_pto_access=not-applicable cpu_signal=0 cleanup=provider-close",
        "mem_service holder-recovery: node=0 status=recovered key=holder-recovery-object state=retired",
    ))
    node_b = "\n".join((
        "mem_service provider-register: status=ok",
        "mem_service holder-recovery: node=1 phase=pto-prepared-observed op_id=8",
        "expired_count=1",
        "mem_service provider-register: status=ok",
        "replaced=0",
        "obmm-holder-fence: status=fenced key=holder-recovery-object generation=1",
        "obmm-holder-fence: status=complete node=guest-node-1 allocations=1",
        "mem_service object-session: action=wait_state key=holder-recovery-object status=ok state=retired",
        "mem_service holder-recovery: node=1 phase=pto-rejected-observed status=ok op_id=8",
        "mem_service holder-recovery: node=1 phase=cpu-probe-start-observed status=ok op_id=8",
        "mem_service holder-recovery: node=1 phase=cpu-rejected-observed status=ok op_id=8",
        "MANAGED_PTO node=1 peer_canary=pass bootstrap_generation=9001",
        "MANAGED_PTO node=1 key=holder-recovery-object generation=1 cpu_base=0x700000000000 address=0x700000000000 logical_bytes=65536 mapping_id=2 mapping_handle=1 access=3 object=input home_node=0 home_provider=guest-node-0 requester_cna=0xf002",
        "MANAGED_OWNER node=1 state=adopted generation=1 mapping_id=2 cpu_pins=1 holder_transferred=1 object=input",
        "MANAGED_PTO_HOLDER_RECOVERY node=1 state=prepared op_id=8 generation=1 mapping_id=2 holder_node=guest-node-1 holder_incarnation=7 retained_leases=3",
        "MANAGED_PTO_HOLDER_RECOVERY node=1 state=retired op_id=8 generation=1 mapping_id=2 retained_leases=3",
        "MANAGED_PTO_HOLDER_RECOVERY node=1 state=submit-start op_id=8 generation=1 mapping_id=2 retained_cpu_pins=1 retained_leases=3",
        "MANAGED_PTO_HOLDER_RECOVERY node=1 state=rejected op_id=8 completion_op=8 status=3 error=pto_ub_gm_bad_memref pending_op=0 cleanup=process-exit",
        "MANAGED_PTO_HOLDER_RECOVERY node=1 state=cpu-probe-start op_id=8 generation=1 mapping_id=2 retained_cpu_pins=1 retained_leases=3",
        "MANAGED_PTO_HOLDER_RECOVERY node=1 state=cpu-rejected op_id=8 generation=1 mapping_id=2 signal=7 code=2 address=0x700000000000 retained_cpu_pins=1 retained_leases=3",
        "MANAGED_PTO_RESULT node=1 status=pass key=holder-recovery-object generation=1 holder_recovery_probe=1 role=stale-holder old_cpu_access=rejected old_pto_access=rejected cpu_signal=7 cleanup=process-exit",
        "mem_service holder-recovery: node=1 status=fenced old_cpu_access=rejected old_pto_access=rejected old_cpu_signal=7",
    ))
    qemu_a = "\n".join((
        "OBMM_COH_FENCE req_id=1 from=0xc4d2 range=0x700000000000+2097152",
        "GSVA_HOME: operation=1 segment=0xc4c2000000000001 epoch=1 export=1 error=0",
        "GSVA_HOME: operation=2 segment=0xc4c2000000000001 epoch=1 export=1 error=0",
    ))
    qemu_b = "\n".join((
        "GSVA_TLB: flush reason=remote_retire_quarantine segment_id=1",
        "OBMM_COH_FENCE_ACK req_id=1 from=0xc4c2 status=0",
        "GSVA_TLB: flush reason=remote_retire_drained segment_id=1",
        "GSVA_UNMAP: map_id=1 segment_id=1 tombstone=yes",
        "GSVA_RECOVERY: local holder revoked cna=1 segment_id=1",
    ))
    return [node_a, node_b, qemu_a, qemu_b]


class HolderRecoveryGuestTests(unittest.TestCase):
    def test_complete_physical_revoke_and_home_retire_evidence(self):
        result = validator.validate(*fixture())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["old_cpu_signal"], 7)
        self.assertEqual(result["old_pto_error"], "pto_ub_gm_bad_memref")
        self.assertEqual(result["old_pto_operation_id"], 8)
        self.assertEqual(result["old_pto_mapping_id"], 2)
        self.assertEqual(result["pto_bootstrap_generation"], 9001)
        self.assertEqual(result["home_retire"], {
            "segment_id": "0xc4c2000000000001",
            "epoch": 1,
            "export_mem_id": 1,
            "worker_generation": 1,
        })

    def test_fence_request_and_ack_must_be_on_correct_qemu_nodes(self):
        logs = fixture()
        logs[2] = logs[2].replace("OBMM_COH_FENCE req_id=1", "unrelated")
        logs[3] += "\nOBMM_COH_FENCE req_id=1"
        with self.assertRaisesRegex(ValueError, "home QEMU"):
            validator.validate(*logs)

        logs = fixture()
        logs[3] = logs[3].replace("OBMM_COH_FENCE_ACK", "unrelated")
        logs[2] += "\nOBMM_COH_FENCE_ACK req_id=1 status=0"
        with self.assertRaisesRegex(ValueError, "holder QEMU"):
            validator.validate(*logs)

    def test_missing_or_unsafe_recovery_evidence_is_rejected(self):
        for index, marker in (
            (0, "state=retiring"),
            (1, "expired_count=1"),
            (1, "obmm-holder-fence: status=fenced"),
            (1, "old_cpu_access=rejected"),
            (1, "state=prepared"),
            (1, "state=submit-start"),
            (1, "state=rejected"),
            (1, "state=cpu-probe-start"),
            (1, "old_pto_access=rejected"),
            (1, "bootstrap_generation=9001"),
            (0, "status=COMMITTED"),
            (2, "operation=2"),
            (3, "remote_retire_drained"),
            (3, "tombstone=yes"),
            (3, "local holder revoked"),
        ):
            with self.subTest(marker=marker):
                logs = fixture()
                logs[index] = logs[index].replace(marker, "missing", 1)
                with self.assertRaises(ValueError):
                    validator.validate(*logs)

        logs = fixture()
        logs[1] += "\naction=read key=holder-recovery-object status=ok"
        with self.assertRaisesRegex(ValueError, "remained readable"):
            validator.validate(*logs)

        logs = fixture()
        logs[1] = logs[1].replace("completion_op=8", "completion_op=9")
        with self.assertRaisesRegex(ValueError, "completion identity"):
            validator.validate(*logs)

        logs = fixture()
        logs[1] = logs[1].replace("bootstrap_generation=9001",
                                  "bootstrap_generation=9002")
        with self.assertRaisesRegex(ValueError, "different bootstrap"):
            validator.validate(*logs)

        logs = fixture()
        logs[0] = logs[0].replace("bootstrap_generation=9001",
                                  "bootstrap_generation=7")
        logs[1] = logs[1].replace("bootstrap_generation=9001",
                                  "bootstrap_generation=7")
        with self.assertRaisesRegex(ValueError, "provider incarnation"):
            validator.validate(*logs)

        logs = fixture()
        observed = next(line for line in logs[1].splitlines()
                        if "phase=pto-prepared-observed" in line)
        logs[1] = logs[1].replace(observed + "\n", "")
        fence = next(line for line in logs[1].splitlines()
                     if "status=fenced" in line)
        logs[1] = logs[1].replace(fence + "\n", fence + "\n" + observed + "\n")
        with self.assertRaisesRegex(ValueError, "after the observed fence"):
            validator.validate(*logs)

        logs = fixture()
        rejected = next(line for line in logs[1].splitlines()
                        if "state=rejected" in line)
        cpu_probe = next(line for line in logs[1].splitlines()
                         if "state=cpu-probe-start" in line)
        logs[1] = logs[1].replace(rejected + "\n", "")
        logs[1] = logs[1].replace(cpu_probe + "\n", cpu_probe + "\n" + rejected + "\n")
        with self.assertRaisesRegex(ValueError, "bounded post-revoke order"):
            validator.validate(*logs)

    def test_home_revoke_must_match_retired_identity_and_success(self):
        for old, new in (
            ("segment=0xc4c2000000000001 epoch=1 export=1 error=0",
             "segment=0xc4c2000000000002 epoch=1 export=1 error=0"),
            ("segment=0xc4c2000000000001 epoch=1 export=1 error=0",
             "segment=0xc4c2000000000001 epoch=2 export=1 error=0"),
            ("segment=0xc4c2000000000001 epoch=1 export=1 error=0",
             "segment=0xc4c2000000000001 epoch=1 export=2 error=0"),
            ("segment=0xc4c2000000000001 epoch=1 export=1 error=0",
             "segment=0xc4c2000000000001 epoch=1 export=1 error=-12"),
        ):
            with self.subTest(replacement=new):
                logs = fixture()
                revoke = "GSVA_HOME: operation=2 " + old
                logs[2] = logs[2].replace(
                    revoke, "GSVA_HOME: operation=2 " + new,
                )
                with self.assertRaisesRegex(ValueError,
                                             "authoritative revoke"):
                    validator.validate(*logs)

    def test_cli_writes_machine_readable_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = []
            for name, content in zip(("node-a", "node-b", "qemu-a", "qemu-b"),
                                     fixture()):
                path = root / name
                path.write_text(content)
                args.extend(("--" + name, str(path)))
            output = root / "report.json"
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "validate_mem_service_holder_recovery.py"),
                 *args, "--output", str(output)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("managed_holder_recovery=pass", result.stdout)
            self.assertEqual(json.loads(output.read_text())["status"], "pass")


if __name__ == "__main__":
    unittest.main()
