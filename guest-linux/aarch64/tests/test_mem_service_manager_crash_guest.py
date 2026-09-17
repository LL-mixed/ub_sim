import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "guest-linux/aarch64/scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location(
    "manager_crash_validator",
    SCRIPTS / "validate_mem_service_manager_crash_guest.py",
)
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)


def fixture():
    key = validator.KEY
    generation = 5
    node_a = "\n".join((
        "mem_service kernel-regions: phase=baseline",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        f"mem_service manager-recovery: node=0 phase=active status=ready "
        f"fault=manager_crash key={key} generation={generation} live_refs=1",
        "mem_service manager-recovery: node=0 phase=manager-exit status=expected "
        "restart=1 signal=9 exit_code=137",
        "mem_service manager-recovery: node=0 phase=worker-exit status=expected "
        "exit_code=1",
        f"mem_service manager-recovery: node=0 phase=restart status=isolated "
        f"restart=1 key={key} generation={generation} state=quarantined "
        "provider_backed=1 live_refs=1 identity_preserved=1 "
        "managed_recovery_required=1",
        "mem_service manager-recovery: node=0 phase=manager-exit status=expected "
        "restart=2 signal=9 exit_code=137",
        f"mem_service manager-recovery: node=0 phase=restart status=isolated "
        f"restart=2 key={key} generation={generation} state=quarantined "
        "provider_backed=1 live_refs=1 identity_preserved=1 "
        "managed_recovery_required=1",
        "mem_service manager-recovery: node=0 phase=directory status=isolated "
        "provider_active_count=2 provider_directory_ready=1 data_plane_ready=0",
        f"mem_service manager-recovery: node=0 phase=old-replay status=rejected "
        f"reason=managed_reconciliation_required key={key} generation={generation}",
        "mem_service manager-recovery: node=0 phase=new-allocation status=rejected "
        "reason=managed_reconciliation_required",
        f"mem_service manager-recovery: node=0 phase=observation status=pass "
        f"fault=manager_crash key={key} generation={generation} state=quarantined "
        "live_refs=1 backing_allocated_bytes=0 address_reserved_bytes=0 "
        "quarantined_bytes=2097152 restarts=2",
        "mem_service kernel-regions: phase=isolated",
        "obmm-kernel-region: id=9 type=export bytes=2097152",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=1 export_bytes=2097152 import_handles=0 import_bytes=0",
        "mem_service object-share: status=ok node=0 role=provider backend=obmm",
    ))
    node_b = "\n".join((
        "mem_service kernel-regions: phase=baseline",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        f"mem_service manager-recovery: node=1 phase=holder status=ready "
        f"fault=manager_crash key={key} generation={generation} live_refs=1",
        "mem_service manager-recovery: node=1 phase=manager-transition status=down "
        "restart=1",
        "mem_service manager-recovery: node=1 phase=worker-exit status=expected "
        "exit_code=1",
        "mem_service manager-recovery: node=1 phase=manager-transition status=up "
        "restart=1",
        "mem_service manager-recovery: node=1 phase=manager-transition status=down "
        "restart=2",
        "mem_service manager-recovery: node=1 phase=manager-transition status=up "
        "restart=2",
        f"mem_service manager-recovery: node=1 phase=observation status=pass "
        f"fault=manager_crash key={key} generation={generation} state=quarantined "
        "live_refs=1 restarts=2",
        "mem_service kernel-regions: phase=isolated",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        "mem_service object-share: status=ok node=1 role=consumer backend=obmm",
    ))
    return node_a, node_b


class ManagerCrashGuestTests(unittest.TestCase):
    def test_repeated_manager_crash_isolated_state_is_accepted(self):
        result = validator.validate(*fixture())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["manager_restarts"], 2)
        self.assertEqual(result["generation"], 5)
        self.assertEqual(result["backing_allocated_bytes"], 0)
        self.assertEqual(result["address_reserved_bytes"], 0)
        self.assertEqual(result["quarantined_bytes"], 2097152)

    def test_missing_or_unsafe_recovery_evidence_is_rejected(self):
        cases = (
            (0, "exit_code=137", "exit_code=1"),
            (0, "identity_preserved=1", "identity_preserved=0"),
            (0, "managed_recovery_required=1", "managed_recovery_required=0"),
            (0, "data_plane_ready=0", "data_plane_ready=1"),
            (0, "reason=managed_reconciliation_required", "reason=data_plane_not_ready"),
            (0, "backing_allocated_bytes=0", "backing_allocated_bytes=1"),
            (0, "address_reserved_bytes=0", "address_reserved_bytes=1"),
            (0, "quarantined_bytes=2097152", "quarantined_bytes=0"),
            (0, "export_handles=1", "export_handles=0"),
            (1, "generation=5", "generation=6"),
            (1, "status=down restart=2", "status=up restart=2"),
            (1, "live_refs=1", "live_refs=0"),
        )
        for node, old, new in cases:
            with self.subTest(old=old):
                logs = list(fixture())
                logs[node] = logs[node].replace(old, new, 1)
                with self.assertRaises((KeyError, ValueError)):
                    validator.validate(*logs)

    def test_worker_fault_receipt_is_rejected(self):
        logs = list(fixture())
        logs[0] += ("\nobmm-worker fault-injection=triggered phase=reserve-intent "
                    "mode=sync-exit ledger_bytes=1024")
        with self.assertRaises(ValueError):
            validator.validate(*logs)

    def test_cli_writes_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            node_a, node_b = fixture()
            (root / "node-a.log").write_text(node_a)
            (root / "node-b.log").write_text(node_b)
            output = root / "report.json"
            run = subprocess.run([
                sys.executable,
                str(SCRIPTS / "validate_mem_service_manager_crash_guest.py"),
                "--node-a", str(root / "node-a.log"),
                "--node-b", str(root / "node-b.log"),
                "--output", str(output),
            ], capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("managed_manager_recovery=pass", run.stdout)
            report = json.loads(output.read_text())
            self.assertEqual(report["manager_exit_code"], 137)
            self.assertTrue(report["managed_recovery_required"])

    def test_guest_and_runner_wire_manager_crash_profile(self):
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        function = run_app.split("mem_service_object_share_manager_recovery() {", 1)[1].split(
            "\nmem_service_object_share_recovery_fault() {", 1
        )[0]
        self.assertIn('kill -9 "$daemon"', function)
        self.assertEqual(function.count("phase=manager-exit status=expected"), 2)
        self.assertIn("managed_recovery_required=1", function)
        self.assertIn("identity_preserved=1", function)
        self.assertIn("phase=old-replay status=rejected", function)
        self.assertIn("phase=new-allocation status=rejected", function)
        self.assertIn("provider_active_count=(0|1)", function)
        self.assertIn("quarantined_bytes", function)
        self.assertIn('mem_service_object_share_manager_recovery "$node"', run_app)
        self.assertIn("mem_service_recovery_fault_supported() {", run_app)
        self.assertIn("reclaimed_crash|manager_crash|guest_crash)", run_app)
        self.assertIn('if [ "$fault" = "manager_crash" ]; then', run_app)
        self.assertIn("validate_mem_service_manager_crash_guest.py", runner)
        self.assertIn("managed-manager-crash-validation.json", runner)


if __name__ == "__main__":
    unittest.main()
