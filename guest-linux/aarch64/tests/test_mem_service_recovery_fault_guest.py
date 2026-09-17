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


def fixture(fault, cycles=1):
    spec = validator.FAULTS[fault]
    mode, integrity = spec["mode"], spec["integrity"]
    logs = []
    for node in (0, 1):
        lines = [
            "mem_service kernel-regions: phase=baseline",
            "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
            "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
        ]
        for cycle in range(1, cycles + 1):
            suffix = validator.cycle_suffix(cycle)
            key = f"recovery-fault-{fault}{suffix}"
            generation = 5 + (cycle - 1) * 4
            prefix = (
                "mem_service object-session: "
                f"session=recovery-fault-observer-{node}{suffix}"
            )
            if node == 0:
                lines += [
                    "obmm-worker initial refresh provider_directory_ready=1",
                    f"obmm-worker fault-injection=triggered phase={spec['phase']} "
                    f"mode={mode} ledger_bytes=2048",
                    "mem_service recovery-fault: node=0 phase=worker-exit "
                    f"status=expected fault={fault} cycle={cycle} "
                    f"injection_phase={spec['phase']} mode={mode} exit_code=86 "
                    f"key={key} generation={generation}",
                ]
                if spec["recovery"] == "quarantined":
                    lines += [
                        "obmm-worker-quarantine: status=complete node=guest-node-0 "
                        f"incarnation=7 provider_state=isolated ledger_integrity={integrity} "
                        "scope=provider-control-plane backing_reconciled=0 "
                        "resource_reconciliation_required=1",
                    ]
                else:
                    reservations = int(spec["resume_state"] == "active")
                    lines += [
                        f"obmm-worker-resume: status=ready reservations={reservations} "
                        "kernel_records=1 unclaimed_records=0 revision=9 "
                        "kernel_instance=0102030405060708090a0b0c0d0e0f10 "
                        "scope=ledger-owned-segment-export-records",
                    ]
                    if spec["resume_state"] == "active":
                        controller = (
                            "mem_service object-session: "
                            f"session=recovery-fault-resume-controller{suffix}"
                        )
                        lines += [
                            f"{controller} op=1 action=wait_state key={key} status=ok "
                            f"state=active generation={generation}",
                            f"{controller} op=2 action=inspect key={key} status=ok "
                            f"state=active generation={generation}",
                            f"{controller} op=3 action=retire key={key} status=ok "
                            f"state=retiring generation={generation}",
                            f"{controller} op=4 action=wait_state key={key} status=ok "
                            f"state=retired generation={generation}",
                            f"{controller} result=ok ops=4 elapsed_ms=7",
                        ]
            in_flight = int(spec["recovery"] == "quarantined")
            quarantined = int(spec["recovery"] == "quarantined")
            active_providers = 1 if spec["recovery"] == "quarantined" else 2
            directory_ready = 0 if spec["recovery"] == "quarantined" else 1
            lines += [
                f"{prefix} op=1 action=wait_state key={key} status=ok "
                f"state={spec['final_state']} generation={generation}",
                f"{prefix} op=2 action=inspect key={key} status=ok "
                f"state={spec['final_state']} generation={generation}",
                f"{prefix} op=3 action=stats status=ok live_objects=0 "
                "backing_allocated_bytes=0 address_reserved_bytes=0 live_refs=0 "
                f"in_flight={in_flight} quarantined_objects={quarantined} "
                "quarantined_bytes=0 import_mappings=0",
                f"{prefix} result=ok ops=3 elapsed_ms=4",
                f"provider_active_count={active_providers}",
                f"provider_directory_ready={directory_ready}",
                "data_plane_ready=0",
                "data_plane_ready=0",
                f"mem_service recovery-fault: node={node} phase=observation "
                f"status=pass fault={fault} cycle={cycle} "
                f"injection_phase={spec['phase']} mode={mode} "
                f"ledger_integrity={integrity} recovery={spec['recovery']} "
                f"final_state={spec['final_state']} key={key}",
            ]
        lines += [
            "mem_service kernel-regions: phase=drained",
            "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
            "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0",
            f"mem_service object-share: status=ok node={node} "
            f"role={'provider' if node == 0 else 'consumer'} backend=obmm",
        ]
        logs.append("\n".join(lines))
    return logs


class RecoveryFaultGuestTests(unittest.TestCase):
    def test_transaction_crashes_are_recovered_as_specified(self):
        for fault, spec in validator.FAULTS.items():
            with self.subTest(fault=fault):
                result = validator.validate(*fixture(fault), fault)
                self.assertEqual(result["status"], "pass")
                self.assertEqual(result["mode"], spec["mode"])
                self.assertEqual(result["ledger_integrity"], spec["integrity"])
                self.assertEqual(result["injection_phase"], spec["phase"])
                self.assertEqual(result["recovery"], spec["recovery"])
                self.assertEqual(result["generation"], 5)

    def test_repeated_resumable_transaction_generations_are_unique(self):
        fault = "published_crash"
        logs = fixture(fault, cycles=2)
        result = validator.validate(*logs, fault, cycles=2)
        self.assertEqual(result["cycles"], 2)
        self.assertEqual(result["generations"], [5, 9])

        duplicate = [text.replace("generation=9", "generation=5")
                     for text in logs]
        with self.assertRaises(ValueError):
            validator.validate(*duplicate, fault, cycles=2)
        with self.assertRaises(ValueError):
            validator.validate(*fixture("reserve_intent_crash"),
                               "reserve_intent_crash", cycles=2)

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

    def test_resumable_phase_requires_resume_and_ready_directory(self):
        fault = "backing_exported_crash"
        cases = (
            (0, "obmm-worker-resume: status=ready", "obmm-worker-resume: status=failed"),
            (0, "state=active generation=5", "state=retired generation=5"),
            (1, "provider_active_count=2", "provider_active_count=1"),
            (1, "provider_directory_ready=1", "provider_directory_ready=0"),
            (1, "in_flight=0", "in_flight=1"),
        )
        for node, old, new in cases:
            with self.subTest(old=old):
                logs = fixture(fault)
                logs[node] = logs[node].replace(old, new, 1)
                with self.assertRaises(ValueError):
                    validator.validate(*logs, fault)

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
        self.assertIn("resume-allocations", function)
        self.assertIn("recovery-fault-resume-controller", function)
        self.assertIn("injection_phase=$injection_phase", function)
        self.assertIn("resource_reconciliation_required=1", function)
        self.assertIn("cycle=$cycle", function)
        self.assertIn("recovery-fault-observer-$node$cycle_suffix", function)
        self.assertIn("managed-share.worker.base.conf", run_app)
        self.assertIn(
            "state_file=/tmp/managed-share.worker$cycle_suffix.state",
            run_app,
        )
        self.assertIn("mem_service_prepare_recovery_fault_worker_config", run_app)
        self.assertIn('"$fault" "$cycle" || exit 1', run_app)
        repeat_block = run_app.split(
            '"$fault" "$cycle" || exit 1', 1
        )[1].split("worker_pid=$!", 1)[0]
        self.assertIn("provider-register", repeat_block)
        self.assertIn('--incarnation "$incarnation"', repeat_block)
        self.assertLess(
            repeat_block.index("provider-register"),
            repeat_block.index("serve-allocations"),
        )
        self.assertIn('final_state="quarantined"', function)
        self.assertIn('final_state="retired"', function)
        self.assertIn("in_flight=1 quarantined_objects=1", function)
        self.assertIn('worker_status" -eq 86', function)
        self.assertNotIn("mem_service_object_share_recovery_fault() (", run_app)
        self.assertIn('MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-recovery-fault"', runner)
        self.assertIn("MEM_SERVICE_OBJECT_SHARE_TRANSACTION_CRASH_CYCLES", runner)
        self.assertIn(
            "mem_service.object_share_transaction_crash_cycles=",
            runner,
        )
        self.assertIn(
            '--cycles "$MEM_SERVICE_OBJECT_SHARE_TRANSACTION_CRASH_CYCLES"',
            runner,
        )
        self.assertIn("validate_mem_service_recovery_fault_guest.py", runner)
        self.assertIn("managed-recovery-fault-validation.json", runner)

    @unittest.skipUnless(shutil.which("zsh"), "zsh required")
    def test_host_and_guest_reject_mismatched_recovery_fault_profiles(self):
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        host_helper = "mem_service_recovery_fault_selected() {" + runner.split(
            "mem_service_recovery_fault_selected() {", 1
        )[1].split("\n}\n", 1)[0] + "\n}\n"
        transaction_helper = "mem_service_transaction_fault_selected() {" + runner.split(
            "mem_service_transaction_fault_selected() {", 1
        )[1].split("\n}\n", 1)[0] + "\n}\n"
        host_marker = 'if [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-recovery-fault" ]]; then'
        host = host_helper + transaction_helper + host_marker + runner.split(
            host_marker, 1
        )[1].split(
            '\nif [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-holder-recovery" ]]; then',
            1,
        )[0]
        guest_helper = "mem_service_recovery_fault_supported() {" + run_app.split(
            "mem_service_recovery_fault_supported() {", 1
        )[1].split("\n}\n", 1)[0] + "\n}\n"
        guest_marker = ('  if [ "$access" = "cpu-recovery-fault" ] &&\n'
                        '      { [ "$backend" != "obmm" ]')
        guest = guest_helper + guest_marker + run_app.split(guest_marker, 1)[1].split(
            '  if [ "$access" = "cpu-holder-recovery" ]; then', 1
        )[0]
        profiles = tuple(
            ("cpu-recovery-fault", "obmm", "65536", "1", fault, True)
            for fault in (*validator.FAULTS, "manager_crash", "guest_crash")
        ) + (
            ("cpu-recovery-fault", "loopback", "65536", "1", "reserve_intent_crash", False),
            ("cpu-recovery-fault", "obmm", "4096", "1", "reserve_intent_crash", False),
            ("cpu-recovery-fault", "obmm", "65536", "2", "reserve_intent_crash", False),
            ("cpu-recovery-fault", "obmm", "65536", "1", "none", False),
            ("cpu", "obmm", "65536", "1", "reserve_intent_crash", False),
            ("cpu", "obmm", "65536", "1", "manager_crash", False),
            ("cpu", "obmm", "65536", "1", "guest_crash", False),
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
                    "MEM_SERVICE_OBJECT_SHARE_GUEST_CRASH_CYCLES=1\n"
                    "MEM_SERVICE_OBJECT_SHARE_TRANSACTION_CRASH_CYCLES=1\n"
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

        transaction_cycle_profiles = (
            ("published_crash", "2", True),
            ("reserve_intent_crash", "2", False),
            ("published_crash", "0", False),
            ("guest_crash", "2", False),
        )
        for fault, cycles, valid in transaction_cycle_profiles:
            with self.subTest(fault=fault, transaction_cycles=cycles):
                host_config = (
                    "MEM_SERVICE_OBJECT_SHARE_ACCESS=cpu-recovery-fault\n"
                    "MEM_SERVICE_OBJECT_SHARE_BACKEND=obmm\n"
                    "MEM_SERVICE_OBJECT_SHARE_SIZE=65536\n"
                    "MEM_SERVICE_OBJECT_SHARE_ITERATIONS=1\n"
                    f"MEM_SERVICE_OBJECT_SHARE_FAULT={fault}\n"
                    "MEM_SERVICE_OBJECT_SHARE_GUEST_CRASH_CYCLES=1\n"
                    f"MEM_SERVICE_OBJECT_SHARE_TRANSACTION_CRASH_CYCLES={cycles}\n"
                )
                run = subprocess.run(
                    ["zsh", "-s"], input=host_config + host,
                    capture_output=True, text=True, timeout=5,
                )
                self.assertEqual(run.returncode == 0, valid,
                                 run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
