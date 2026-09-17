import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "guest-linux/aarch64/scripts"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("retired_validator", SCRIPTS / "validate_mem_service_retired_guest.py")
validator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validator)
ADDRESS = 0x700000000000
FIELDS = ("live_objects", "live_refs", "in_flight", "import_mappings", "backing_allocated_bytes",
          "address_reserved_bytes", "quarantined_objects", "quarantined_bytes")


def fixture():
    logs = []
    census = "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic export_handles=0 export_bytes=0 import_handles=0 import_bytes=0"
    resources = ("mem_service provider-resources: provider=obmm scope=endpoint version=1 "
                 "export_handles=1 export_bytes=2097152 import_handles=0 import_bytes=0 "
                 "vma_count=0 vma_bytes=0 accessible_views=0 accessible_bytes=0 "
                 "cleanup_mappings=0 closing=0 control_close_uncertain=0")
    for node in (0, 1):
        prefix = f"mem_service object-session: session=retired-client-{node} "
        lines = ["mem_service kernel-regions: phase=baseline", census, prefix + "peer_canary=ok peers=1"]
        operations = validator.expected_operations(node)
        for index, (action, key) in enumerate(operations, 1):
            row = dict(op=index, action=action, key=key, status="ok")
            if action == "acquire":
                row["generation"] = 1 if key == "retired-old" else 4
            if action == "wait_state":
                retired = (node == 1 and key.endswith("-done")) or (
                    node == 0 and index > 1 and operations[index - 2][0] == "retire")
                row["state"] = "retired" if retired else "active"
            if action == "map":
                row.update(base=hex(ADDRESS), len=65536)
            if action in ("read", "write", "publish_data", "wait_visible"):
                seed = 41 if key == "retired-old" else 42
                row.update(offset=0, len=65536, checksum=hex(validator.checksum(
                    bytes((seed + i) % 256 for i in range(65536)))))
            if action == "unmap":
                generation = 1 if key == "retired-old" else 4
                lines.append(prefix + f"cpu_unmapped_probe=pass key={key} generation={generation} "
                             f"address={hex(ADDRESS)} signal=11 read_before_unmap=1 unmap_confirmed=1")
            if action == "capture_mapping":
                lines.append(prefix + f"mapping_capture=ok key=retired-old generation=1 "
                             f"address={hex(ADDRESS)} len=65536 descriptor_checksum=0x123")
            if action == "probe_retired_mapping":
                lines.append(prefix + "retired_mapping_probe=pass key=retired-old generation=1 "
                             f"replacement_key=retired-new replacement_generation=4 address={hex(ADDRESS)} "
                             "map_result=0 map_errno=0 signal=11 unmap_confirmed=1")
            if action == "stats":
                row.update({name: 0 for name in FIELDS})
            lines.append(prefix + " ".join(f"{name}={value}" for name, value in row.items()))
            if action == "stats":
                lines.append(resources)
        lines += [prefix + f"result=ok ops={len(operations)} elapsed_ms=30",
                  "mem_service kernel-regions: phase=drained", census,
                  f"mem_service object-share: status=ok node={node} role={'provider' if node == 0 else 'consumer'} backend=obmm"]
        logs.append("\n".join(lines))
    qemu = []
    for index, segment in enumerate((10, 20, 10, 20), 1):
        identity = f"map_id={index} segment_id={segment} home_va={hex(ADDRESS)} epoch=1"
        qemu += [f"GSVA_MAP: {identity} size=0x200000 source=2 profile=1",
                 "GSVA_MAP: cpu_window registered at pa=1000 size=200000",
                 f"GSVA_UNMAP: {identity} tombstone=no", "GSVA_UNMAP: cpu_window removed pa=1000"]
    return [*logs, "\n".join(qemu)]


class RetiredGuestTests(unittest.TestCase):
    def test_complete_two_generation_evidence(self):
        result = validator.validate(*fixture(), 41)
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["generations"], {"retired-old": 1, "retired-new": 4})
        self.assertEqual(len(result["reader_routes"]), 4)

    def test_bad_fault_identity_cleanup_or_payload_is_rejected(self):
        for old, new in (("retired_mapping_probe=pass", "retired_mapping_probe=fail"),
                         ("map_result=0", "map_result=-1"), ("signal=11", "signal=0"),
                         ("unmap_confirmed=1", "unmap_confirmed=0"),
                         ("replacement_generation=4", "replacement_generation=1"),
                         ("mapping_capture=ok", "mapping_capture=fail"),
                         ("generation=1", "generation=5"),
                         ("checksum=0x", "checksum=0x1"),
                         ("import_handles=0", "import_handles=1"),
                         ("live_refs=0", "live_refs=1")):
            with self.subTest(old=old):
                logs = fixture()
                logs[1] = logs[1].replace(old, new, 1)
                # Final global counters are checked only at final quiescence.
                if old == "live_refs=0":
                    logs[1] = logs[1].replace(old, new)
                with self.assertRaises(ValueError):
                    validator.validate(*logs, 41)

    def test_missing_reordered_and_wrong_route_receipts_are_rejected(self):
        for node, needle in ((1, "mapping_capture=ok"), (1, "retired_mapping_probe=pass"),
                             (1, "action=read key=retired-new"), (0, "result=ok"),
                             (2, "GSVA_UNMAP: map_id=3")):
            logs = fixture()
            lines = logs[node].splitlines()
            index = next(i for i, line in enumerate(lines) if needle in line)
            lines.pop(index)
            logs[node] = "\n".join(lines)
            with self.assertRaises(ValueError):
                validator.validate(*logs, 41)
        logs = fixture()
        logs[2] = logs[2].replace("map_id=3 segment_id=10", "map_id=3 segment_id=20")
        with self.assertRaises(ValueError):
            validator.validate(*logs, 41)
        logs = fixture()
        logs[2] = logs[2].replace("profile=1", "profile=0", 1)
        with self.assertRaises(ValueError):
            validator.validate(*logs, 41)

    def test_guest_generated_operations_match_validator_and_single_session(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        function = "mem_service_object_share_retired_ops() {" + source.split(
            "mem_service_object_share_retired_ops() {", 1)[1].split(
            "\nmem_service_object_share_retired()", 1)[0]
        for node in (0, 1):
            run = subprocess.run(["sh", "-s"], input=function +
                                 f"\nmem_service_object_share_retired_ops {node} 65536 41\n",
                                 capture_output=True, text=True, timeout=5, check=True)
            rows = [dict(item.split("=", 1) for item in line.split()) for line in run.stdout.splitlines()]
            self.assertEqual([(row["op"], row.get("key", "-")) for row in rows],
                             validator.expected_operations(node))
            self.assertLessEqual(len(rows), 64)
            self.assertTrue(all(row["probe_unmapped"] == "1" for row in rows if row["op"] == "unmap"))
        harness = source.split("mem_service_object_share_retired() (", 1)[1].split(
            "\nmem_service_object_share_wait_infrastructure()", 1)[0]
        self.assertEqual(harness.count("/bin/linqu_mem_service object-session"), 1)
        self.assertNotIn("request_timeout_ms=", harness)

    def test_cli_emits_report_and_rejects_corrupt_input(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = []
            for name, content in zip(("node-a", "node-b", "qemu-b"), fixture()):
                path = root / name
                path.write_text(content)
                args += ["--" + name, str(path)]
            command = [sys.executable, str(SCRIPTS / "validate_mem_service_retired_guest.py"),
                       *args, "--seed", "41", "--output", str(root / "report.json")]
            run = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("managed_retired=pass", run.stdout)
            (root / "qemu-b").write_text("")
            run = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 1, run.stderr)

    @unittest.skipUnless(shutil.which("zsh"), "zsh required")
    def test_host_and_guest_reject_invalid_retired_profiles(self):
        host_source = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        marker = 'if [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-retired" ]]; then'
        host = marker + host_source.split(marker, 1)[1].split(
            '\nif [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-reference" ]]; then', 1)[0]
        guest_source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        marker = '  if [ "$access" = "cpu-retired" ] &&'
        guest = marker + guest_source.split(marker, 1)[1].split(
            '  if [ "$access" = "cpu-reference" ] &&', 1)[0]
        for backend, size, iterations, fault, valid in (
                ("obmm", "65536", "1", "none", True),
                ("loopback", "65536", "1", "none", False),
                ("obmm", "4096", "1", "none", False),
                ("obmm", "65536", "2", "none", False),
                ("obmm", "65536", "01", "none", False),
                ("obmm", "65536", "1", "capacity", False)):
            with self.subTest(backend=backend, size=size, iterations=iterations, fault=fault):
                config = dict(access="cpu-retired", backend=backend, size=size,
                              iterations=iterations, fault=fault)
                for shell, script in (
                        ("zsh", "\n".join(f"MEM_SERVICE_OBJECT_SHARE_{key.upper()}={value}"
                                          for key, value in config.items()) + "\n" + host),
                        ("sh", "\n".join(f"{key}={value}" for key, value in config.items()) +
                         "\nlog() { :; }\ncheck() {\n" + guest + "\n}\ncheck\n")):
                    run = subprocess.run([shell, "-s"], input=script, capture_output=True,
                                         text=True, timeout=5)
                    self.assertEqual(run.returncode == 0, valid, run.stderr)


if __name__ == "__main__":
    unittest.main()
