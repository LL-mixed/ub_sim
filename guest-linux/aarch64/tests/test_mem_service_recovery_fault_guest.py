import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "guest-linux/aarch64/scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location(
    "recovery_fault_validator",
    SCRIPTS / "validate_mem_service_recovery_fault_guest.py",
)
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


def fixture(fault):
    mode, integrity = validator.FAULTS[fault]
    key = f"recovery-fault-{fault}"
    logs = []
    for node in (0, 1):
        prefix = f"mem_service object-session: session=recovery-fault-observer-{node}"
        lines = [
            "mem_service kernel-regions: phase=baseline",
            "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
            "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        ]
        if node == 0:
            lines += [
                f"obmm-worker fault-injection=triggered phase=reserve-intent "
                f"mode={mode} ledger_bytes=2048",
                f"mem_service recovery-fault: node=0 phase=worker-exit status=expected "
                f"fault={fault} mode={mode} exit_code=86 key={key} generation=5",
                "obmm-worker-quarantine: status=complete node=guest-node-0 "
                f"incarnation=7 provider_state=isolated ledger_integrity={integrity} "
                "scope=provider-control-plane backing_reconciled=0 "
                "resource_reconciliation_required=1",
            ]
        lines += [
            f"{prefix} op=1 action=wait_state key={key} status=ok "
            "state=quarantined generation=5",
            f"{prefix} op=2 action=inspect key={key} status=ok "
            "state=quarantined generation=5",
            f"{prefix} op=3 action=stats status=ok live_objects=0 "
            "backing_allocated_bytes=0 address_reserved_bytes=0 live_refs=0 "
            "in_flight=1 quarantined_objects=1 quarantined_bytes=0 import_mappings=0",
            f"{prefix} result=ok ops=3 elapsed_ms=4",
            "provider_active_count=1",
            "provider_directory_ready=0",
            "data_plane_ready=0",
            "data_plane_ready=0",
            f"mem_service recovery-fault: node={node} phase=observation status=pass "
            f"fault={fault} mode={mode} ledger_integrity={integrity} key={key}",
            "mem_service kernel-regions: phase=drained",
            "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
            "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
            f"mem_service object-share: status=ok node={node} "
            f"role={'provider' if node == 0 else 'consumer'} backend=obmm",
        ]
        logs.append("\n".join(lines))
    return logs


class RecoveryFaultGuestTests(unittest.TestCase):
    def test_complete_and_torn_crashes_are_isolated(self):
        for fault, expected in validator.FAULTS.items():
            with self.subTest(fault=fault):
                result = validator.validate(*fixture(fault), fault)
                self.assertEqual(result["status"], "pass")
                self.assertEqual(result["mode"], expected[0])
                self.assertEqual(result["ledger_integrity"], expected[1])
                self.assertEqual(result["generation"], 5)

    def test_missing_or_incorrect_isolation_evidence_is_rejected(self):
        fault = "reserve_intent_crash"
        cases = (
            (0, "exit_code=86", "exit_code=1"),
            (0, "provider_state=isolated", "provider_state=replaced"),
            (0, "ledger_integrity=complete", "ledger_integrity=torn"),
            (0, "resource_reconciliation_required=1", "resource_reconciliation_required=0"),
            (0, "ledger_bytes=2048", "ledger_bytes=0"),
            (1, "generation=5", "generation=6"),
            (1, "in_flight=1", "in_flight=0"),
            (1, "quarantined_objects=1", "quarantined_objects=0"),
            (1, "data_plane_ready=0", "data_plane_ready=1"),
            (1, "provider_active_count=1", "provider_active_count=2"),
        )
        for node, old, new in cases:
            with self.subTest(old=old):
                logs = fixture(fault)
                logs[node] = logs[node].replace(old, new, 1)
                with self.assertRaises(ValueError):
                    validator.validate(*logs, fault)

    def test_fault_cannot_appear_on_peer(self):
        logs = fixture("torn_ledger_crash")
        logs[1] += "\nobmm-worker fault-injection=triggered phase=reserve-intent " \
                   "mode=torn-exit ledger_bytes=2048"
        with self.assertRaises(ValueError):
            validator.validate(*logs, "torn_ledger_crash")

    def test_cli_writes_report_and_rejects_wrong_fault(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            node_a, node_b = fixture("reserve_intent_crash")
            (root / "node-a.log").write_text(node_a)
            (root / "node-b.log").write_text(node_b)
            output = root / "report.json"
            command = [
                sys.executable,
                str(SCRIPTS / "validate_mem_service_recovery_fault_guest.py"),
                "--node-a", str(root / "node-a.log"),
                "--node-b", str(root / "node-b.log"),
                "--fault", "reserve_intent_crash",
                "--output", str(output),
            ]
            run = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("managed_recovery_fault=pass", run.stdout)
            self.assertEqual(json.loads(output.read_text())["worker_exit_code"], 86)
            command[command.index("reserve_intent_crash")] = "torn_ledger_crash"
            run = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 1, run.stdout + run.stderr)

    def test_guest_and_runner_wire_only_the_dedicated_profile(self):
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        function = run_app.split("mem_service_object_share_recovery_fault() {", 1)[1].split(
            "\nmem_service_object_share_managed() (", 1)[0]
        self.assertIn("quarantine-allocation-state", function)
        self.assertIn("resource_reconciliation_required=1", function)
        self.assertIn("state=quarantined", function)
        self.assertIn("in_flight=1 quarantined_objects=1", function)
        self.assertIn('worker_status" -eq 86', function)
        self.assertNotIn("mem_service_object_share_recovery_fault() (", run_app)
        self.assertIn('MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-recovery-fault"', runner)
        self.assertIn("validate_mem_service_recovery_fault_guest.py", runner)
        self.assertIn("managed-recovery-fault-validation.json", runner)

    @unittest.skipUnless(shutil.which("zsh"), "zsh required")
    def test_host_and_guest_reject_mismatched_recovery_fault_profiles(self):
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        host_marker = 'if [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-recovery-fault" ]]; then'
        host = host_marker + runner.split(host_marker, 1)[1].split(
            '\nif [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-holder-recovery" ]]; then', 1
        )[0]
        guest_marker = ('  if [ "$access" = "cpu-recovery-fault" ] &&\n'
                        '      { [ "$backend" != "obmm" ]')
        guest = guest_marker + run_app.split(guest_marker, 1)[1].split(
            '  if [ "$access" = "cpu-holder-recovery" ]; then', 1
        )[0]
        profiles = (
            ("cpu-recovery-fault", "obmm", "65536", "1", "reserve_intent_crash", True),
            ("cpu-recovery-fault", "obmm", "65536", "1", "torn_ledger_crash", True),
            ("cpu-recovery-fault", "loopback", "65536", "1", "reserve_intent_crash", False),
            ("cpu-recovery-fault", "obmm", "4096", "1", "reserve_intent_crash", False),
            ("cpu-recovery-fault", "obmm", "65536", "2", "reserve_intent_crash", False),
            ("cpu-recovery-fault", "obmm", "65536", "1", "none", False),
            ("cpu", "obmm", "65536", "1", "reserve_intent_crash", False),
            ("cpu-holder-recovery", "obmm", "65536", "1", "torn_ledger_crash", False),
        )
        for access, backend, size, iterations, fault, valid in profiles:
            with self.subTest(access=access, backend=backend, size=size,
                              iterations=iterations, fault=fault):
                host_config = (
                    f"MEM_SERVICE_OBJECT_SHARE_ACCESS={access}\n"
                    f"MEM_SERVICE_OBJECT_SHARE_BACKEND={backend}\n"
                    f"MEM_SERVICE_OBJECT_SHARE_SIZE={size}\n"
                    f"MEM_SERVICE_OBJECT_SHARE_ITERATIONS={iterations}\n"
                    f"MEM_SERVICE_OBJECT_SHARE_FAULT={fault}\n"
                )
                guest_config = (
                    f"access={access}\nbackend={backend}\nsize={size}\n"
                    f"iterations={iterations}\nfault={fault}\n"
                )
                for shell, script in (
                    ("zsh", host_config + host),
                    ("sh", guest_config + "log() { :; }\ncheck() {\n" + guest + "\n}\ncheck\n"),
                ):
                    run = subprocess.run([shell, "-s"], input=script, capture_output=True,
                                         text=True, timeout=5)
                    self.assertEqual(run.returncode == 0, valid,
                                     f"{shell}: {run.stdout}{run.stderr}")


if __name__ == "__main__":
    unittest.main()
