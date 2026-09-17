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
    "guest_crash_validator",
    SCRIPTS / "validate_mem_service_guest_crash_guest.py",
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
        f"mem_service guest-recovery: node=0 phase=active status=ready "
        f"fault=guest_crash key={key} generation={generation} old_incarnation=7 "
        "live_refs=1 import_mappings=1",
        f"mem_service guest-recovery: node=0 phase=guest-down status=isolated "
        f"fault=guest_crash key={key} generation={generation} old_incarnation=7 "
        "state=quarantined live_refs=1 import_mappings=1 expired_count=1 "
        "managed_recovery_required=1 data_plane_ready=0 identity_preserved=1",
        f"mem_service guest-recovery: node=0 phase=replacement status=isolated "
        f"fault=guest_crash key={key} generation={generation} old_incarnation=7 "
        "new_incarnation=8 state=quarantined provider_active_count=2 "
        "provider_directory_ready=1 data_plane_ready=0 identity_preserved=1",
        f"mem_service guest-recovery: node=0 phase=old-replay status=rejected "
        f"reason=managed_reconciliation_required key={key} generation={generation} "
        "old_incarnation=7",
        "mem_service guest-recovery: node=0 phase=new-allocation status=rejected "
        "reason=managed_reconciliation_required",
        f"mem_service guest-recovery: node=0 phase=observation status=pass "
        f"fault=guest_crash key={key} generation={generation} old_incarnation=7 "
        "new_incarnation=8 state=quarantined live_refs=1 import_mappings=1 "
        "backing_allocated_bytes=0 address_reserved_bytes=0 "
        "quarantined_bytes=2097152 managed_recovery_required=1 data_plane_ready=0",
        "mem_service kernel-regions: phase=isolated",
        "obmm-kernel-region: id=9 type=export bytes=2097152",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=1 export_bytes=2097152 import_handles=0 import_bytes=0",
        "mem_service object-share: status=ok node=0 role=provider backend=obmm",
    ))
    node_b_before = "\n".join((
        f"mem_service guest-recovery: node=1 phase=crash-ready status=ready "
        f"fault=guest_crash key={key} generation={generation} incarnation=7 "
        "address=123145302310912 live_refs=1 import_mappings=1 mapping=active",
    ))
    node_b_after = "\n".join((
        "mem_service kernel-regions: phase=baseline",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        f"mem_service guest-recovery: node=1 phase=replacement status=isolated "
        f"fault=guest_crash key={key} generation={generation} incarnation=8 "
        "state=quarantined managed_recovery_required=1 data_plane_ready=0",
        "mem_service kernel-regions: phase=isolated",
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        "mem_service object-share: status=ok node=1 role=consumer backend=obmm",
    ))
    qemu_b_before = (
        "GSVA_MAP: map_id=5 segment_id=14177894576915742721 "
        "home_va=0x700000000000 epoch=1 size=2097152 source=2 profile=1\n"
    )
    host_log = "\n".join((
        "mem_service guest-recovery-host: phase=guest-kill node=1 signal=9 pid=123",
        "mem_service guest-recovery-host: phase=guest-exit node=1 status=expected "
        "signal=9 exit_code=137",
        "mem_service guest-recovery-host: phase=guest-restart node=1 "
        "old_incarnation=7 new_incarnation=8",
    ))
    return node_a, node_b_before, node_b_after, qemu_b_before, host_log


class GuestCrashGuestTests(unittest.TestCase):
    def test_guest_kill_restart_isolation_is_accepted(self):
        result = validator.validate(*fixture())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["guest_exit_code"], 137)
        self.assertEqual(result["old_incarnation"], 7)
        self.assertEqual(result["new_incarnation"], 8)
        self.assertEqual(result["import_mappings"], 1)
        self.assertEqual(result["quarantined_bytes"], 2097152)

    def test_missing_or_unsafe_guest_recovery_evidence_is_rejected(self):
        cases = (
            (0, "identity_preserved=1", "identity_preserved=0"),
            (0, "data_plane_ready=0", "data_plane_ready=1"),
            (0, "expired_count=1", "expired_count=0"),
            (0, "quarantined_bytes=2097152", "quarantined_bytes=0"),
            (1, "mapping=active", "mapping=missing"),
            (2, "incarnation=8", "incarnation=7"),
            (3, "home_va=0x700000000000", "home_va=0x700000200000"),
            (3, "source=2", "source=1"),
            (4, "phase=guest-kill", "phase=guest-skip"),
            (4, "pid=123", "pid=0"),
            (4, "exit_code=137", "exit_code=143"),
            (4, "new_incarnation=8", "new_incarnation=7"),
        )
        for log_index, old, new in cases:
            with self.subTest(old=old):
                logs = list(fixture())
                logs[log_index] = logs[log_index].replace(old, new, 1)
                with self.assertRaises((KeyError, ValueError)):
                    validator.validate(*logs)

    def test_cli_writes_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            names = ("node-a", "node-b-before", "node-b-after",
                     "qemu-b-before", "host")
            paths = []
            for name, content in zip(names, fixture()):
                path = root / f"{name}.log"
                path.write_text(content)
                paths.append(path)
            output = root / "report.json"
            run = subprocess.run([
                sys.executable,
                str(SCRIPTS / "validate_mem_service_guest_crash_guest.py"),
                "--node-a", str(paths[0]),
                "--node-b-before", str(paths[1]),
                "--node-b-after", str(paths[2]),
                "--qemu-b-before", str(paths[3]),
                "--host-log", str(paths[4]),
                "--output", str(output),
            ], capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("managed_guest_recovery=pass", run.stdout)
            report = json.loads(output.read_text())
            self.assertTrue(report["managed_recovery_required"])
            self.assertEqual(report["scope"],
                             "actual-guest-sigkill-restart-isolation")

    def test_guest_and_runner_wire_guest_crash_profile(self):
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        manager = (
            ROOT / "guest-linux/aarch64/apps/gva_manager/gva_manager.c"
        ).read_text()
        manager_header = (
            ROOT / "guest-linux/aarch64/libs/gva_manager/gva_manager.h"
        ).read_text()
        function = run_app.split("mem_service_object_share_guest_recovery() {", 1)[1].split(
            "\nmem_service_object_share_recovery_fault() {", 1
        )[0]
        self.assertIn("phase=crash-ready status=ready", function)
        self.assertIn("session_id=guest-recovery-home", function)
        self.assertEqual(function.count("provider_generation=$((incarnation + 101))"), 2)
        self.assertIn("import_mappings=1 mapping=active", function)
        self.assertIn("timeout_ms=60000 poll_ms=100", function)
        self.assertNotIn("timeout_ms=600000", function)
        self.assertIn("expired_count=1", function)
        self.assertIn("new_incarnation=$replacement_incarnation", function)
        self.assertIn("managed_reconciliation_required", function)
        self.assertIn("phase=rejoin-bootstrap status=start", function)
        self.assertIn('--generation "$incarnation"', function)
        self.assertIn(
            '--bootstrap-generation "$replacement_incarnation" --reuse-aperture',
            function,
        )
        self.assertIn(
            '--node-count 2 --generation "$replacement_incarnation"',
            function,
        )
        self.assertIn("phase=rejoin-bootstrap status=ready", function)
        self.assertIn('aperture_generation="$incarnation"', run_app)
        self.assertIn('bootstrap_generation="$incarnation"', run_app)
        self.assertIn('--generation "$aperture_generation"', run_app)
        self.assertIn('--bootstrap-generation "$bootstrap_generation"', run_app)
        self.assertIn('cfg->bootstrap_generation', manager)
        self.assertIn('cfg->reuse_aperture', manager)
        self.assertIn("gva_manager_validate_aperture", manager)
        self.assertIn("gva_manager_validate_aperture", manager_header)
        self.assertIn('kill -9 "$guest_pid"', runner)
        self.assertIn("mem_service.object_share_guest_restart=1", runner)
        restart_block = runner.split(
            "phase=guest-restart", 1
        )[1].split("if ! wait_for_fm_links_ready", 1)[0]
        self.assertNotIn("ub_link: connected to remote server", restart_block)
        self.assertIn(
            "ub_link: apply nodeA\\.ubcdev0:1 <-> ubcdev0:1 "
            "\\(shmem=1 sock=1 guid=1 snap=1",
            restart_block,
        )
        self.assertIn("validate_mem_service_guest_crash_guest.py", runner)
        self.assertIn("managed-guest-crash-validation.json", runner)


if __name__ == "__main__":
    unittest.main()
