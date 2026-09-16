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
        "mem_service object-session: session=holder-recovery-controller peer_canary=ok peers=1",
        "mem_service recover-allocation: status=ok",
        "state=retiring",
        "mem_service object-session: action=wait_state key=holder-recovery-object status=ok state=retired",
        "mem_service holder-recovery: node=0 status=recovered key=holder-recovery-object state=retired",
    ))
    node_b = "\n".join((
        "mem_service provider-register: status=ok",
        "mem_service object-session: session=holder-recovery-client-1 peer_canary=ok peers=1",
        "mem_service object-session: action=map key=holder-recovery-object status=ok",
        "mem_service object-session: action=publish_data key=holder-recovery-object status=ok",
        "expired_count=1",
        "mem_service provider-register: status=ok",
        "replaced=0",
        "obmm-holder-fence: status=fenced key=holder-recovery-object generation=1",
        "obmm-holder-fence: status=complete node=guest-node-1 allocations=1",
        "mem_service object-session: action=wait_state key=holder-recovery-object status=ok state=retired",
        "mem_service holder-recovery: node=1 status=fenced old_cpu_access=rejected signal_exit=135",
    ))
    qemu_a = "\n".join((
        "OBMM_COH_FENCE req_id=1 from=0xc4d2 range=0x700000000000+2097152",
        "GSVA_COH: home retire route finalized segment_id=1",
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
        self.assertEqual(result["old_cpu_signal_exit"], 135)

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
            (2, "home retire route finalized"),
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
