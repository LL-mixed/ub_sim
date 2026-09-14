"""Synthetic validator and guest command-generation tests; no guest execution."""
import importlib.util
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "guest-linux/aarch64/scripts"
SCRIPT = SCRIPTS / "validate_mem_service_reference_guest.py"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("reference_guest_validator", SCRIPT)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
sys.path.pop(0)


def fixture(iterations=1):
    """Deliberately synthetic logs, including independently encoded references."""
    logs = []
    for node in (0, 1):
        lines = []
        for phase in ("baseline",):
            lines.extend([f"mem_service kernel-regions: phase={phase}",
                          "obmm-kernel-region: id=7 type=export bytes=2097152",
                          "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
                          "export_handles=1 export_bytes=2097152 import_handles=0 import_bytes=0"])
        for round_index in range(1, iterations + 1):
            key = f"obj-ref-{round_index}"
            # Values intentionally do not follow the driver's allocation order.
            generation = 73 + 7 * round_index
            base = 0x700000000000 + round_index * 0x600000
            session = f"mem_service object-session: session=client-{node}"
            lines.append(session + " peer_canary=ok peers=1")
            if node == 0:
                lines.append(f"mem_service reference-allocation: key={key} generation={generation} version=8")
            map_index = 0
            read_index = 0
            visible_index = 0
            write_index = 0
            stats_index = 0
            sizes = [0, 65536, 0, 0] if node == 0 else [0, 512, 0, 512, 0, 0]
            operations = validator.expected_operations(node, key)
            for index, (action, op_key, status) in enumerate(operations, 1):
                line = f"{session} op={index} action={action}"
                if action != "stats":
                    line += f" key={op_key}"
                line += f" status={status}"
                if action == "publish_reference":
                    view_index = int(op_key.endswith("/b"))
                    offset = (128, 4224)[view_index]
                    data = bytes((41 + view_index + i) & 255 for i in range(512))
                    ref = struct.pack("<QHHHHIIQQQQQIIQQQ96s64s", 0x514f424d4d524546,
                        2, 5, 2, 0, 0, 0, 9, validator.checksum(key.encode()), offset, 512,
                        validator.checksum(data), 256, 1, generation, 7, 65536,
                        key.encode(), b"guest-node-0").hex()
                    lines.append(f"{session} reference_key={op_key} reference_hex={ref}")
                if action == "map":
                    line += f" base=0x{base:016x} len=65536"
                elif action == "map_reference":
                    line += f" base=0x{base + (128, 4224)[map_index]:016x} len=512"
                    map_index += 1
                elif action in ("write", "read", "wait_visible"):
                    if status != "ok":
                        line += " note=" + ("reference_payload_frozen" if node == 0 else "out_of_bounds")
                    else:
                        view_index = {"write": write_index, "read": read_index,
                                      "wait_visible": visible_index}[action]
                        offset = (128, 4224)[view_index] if node == 0 else 0
                        value = validator.checksum(bytes((41 + view_index + i) & 255 for i in range(512)))
                        line += f" offset={offset} len=512 checksum=0x{value:016x}"
                        write_index += int(action == "write")
                        read_index += int(action == "read")
                        visible_index += int(action == "wait_visible")
                elif action == "probe_readonly":
                    lines.append(session + " cpu_fault_probe=probe_readonly offset=0 signal=11")
                elif action == "stats":
                    size = sizes[stats_index]
                    stats_index += 1
                    live = int(size != 0)
                    line += (f" live_objects=0 backing_allocated_bytes=0 address_reserved_bytes=0 "
                             f"live_refs=0 in_flight=0 quarantined_objects=0 quarantined_bytes=0 import_mappings={live}")
                else:
                    state = "active"
                    if action == "wait_state" and (node == 0 and index > 19 or node == 1 and index == len(operations) - 1):
                        state = "retired"
                    line += f" state={state} generation={generation}"
                lines.append(line)
                if action == "stats":
                    lines.append("mem_service provider-resources: provider=obmm scope=endpoint version=1 "
                        f"export_handles=1 export_bytes=2097152 import_handles={live * node} "
                        f"import_bytes={live * node * 2097152} vma_count={live * 2} "
                        f"vma_bytes={live * 2097152} accessible_views={live} accessible_bytes={size} "
                        "cleanup_mappings=0 closing=0 control_close_uncertain=0")
            lines.append(f"{session} result=ok ops={len(operations)} elapsed_ms=1")
        lines.extend(["mem_service kernel-regions: phase=drained",
                      "obmm-kernel-region: id=7 type=export bytes=2097152",
                      "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
                      "export_handles=1 export_bytes=2097152 import_handles=0 import_bytes=0",
                      f"mem_service object-share: status=ok node={node} "
                      f"role={'provider' if node == 0 else 'consumer'} backend=obmm"])
        logs.append("\n".join(lines) + "\n")
    return logs


def qemu_fixture(iterations=1):
    lines = []
    for round_index in range(1, iterations + 1):
        base = 0x700000000000 + round_index * 0x600000
        for view in range(2):
            mapping = (round_index - 1) * 2 + view + 1
            identity = f"map_id={mapping} segment_id=0x{round_index + 100:x} home_va=0x{base:x}"
            lines.extend([
                f"GSVA_MAP: {identity} size=0x200000 epoch=3 p_tag=12 cache_policy=1 source=2 profile=1",
                "GSVA_MAP: cpu_window registered at pa=40000000000 size=200000",
                "GSVA_UNMAP: cpu_window removed from pa=40000000000",
                f"GSVA_UNMAP: {identity} epoch=3 tombstone=no",
            ])
    return "\n".join(lines) + "\n"


class ReferenceGuestTests(unittest.TestCase):
    def test_hundred_distinct_allocation_lifecycles(self):
        logs = fixture(100)
        result = validator.validate(*logs, 100, 41, 7)
        self.assertEqual(result["allocation_cycles"], 100)
        self.assertEqual(result["retired_allocations"], 300)
        self.assertEqual(len(result["views"]), 200)
        self.assertEqual(len({view["generation"] for view in result["views"]}), 100)
        self.assertEqual(len(validator.validate_qemu(qemu_fixture(100), result["views"])), 200)
        for count in (0, 101):
            with self.assertRaisesRegex(ValueError, "unsupported reference"):
                validator.validate(*logs, count, 41, 7)
        with self.assertRaisesRegex(ValueError, "missing successful session completion"):
            validator.validate(*fixture(99), 100, 41, 7)

    def test_every_writer_round_must_drain_before_reallocation(self):
        logs = fixture(3)
        lines = logs[0].splitlines()
        stats = [index for index, line in enumerate(lines) if "action=stats" in line]
        for field in ("live_objects", "live_refs", "in_flight", "import_mappings",
                      "backing_allocated_bytes", "address_reserved_bytes",
                      "quarantined_objects", "quarantined_bytes"):
            with self.subTest(field=field):
                changed = lines.copy()
                changed[stats[3]] = changed[stats[3]].replace(f"{field}=0", f"{field}=1")
                with self.assertRaises(ValueError):
                    validator.validate("\n".join(changed), logs[1], 3, 41, 7)

    @unittest.skipUnless(shutil.which("zsh"), "zsh runner required")
    def test_host_and_guest_iteration_bounds_agree(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        guest = "mem_service_reference_iterations_valid() {" + source.split(
            "mem_service_reference_iterations_valid() {", 1)[1].split(
            "\nmem_service_object_share_reference_ops()", 1)[0]
        host = (SCRIPTS / "run_ub_dual_node_apps.sh").read_text()
        marker = 'if [[ "$MEM_SERVICE_OBJECT_SHARE_ACCESS" == "cpu-reference" ]]; then'
        host = marker + host.split(marker, 1)[1].split("\nif managed_pto_selected;", 1)[0]
        for value, valid in (("1", True), ("2", True), ("3", True), ("100", True),
                             ("0", False), ("01", False), ("101", False),
                             ("-1", False), ("1x", False), ("", False)):
            with self.subTest(value=value):
                for shell, script in (("sh", guest +
                        f"\nmem_service_reference_iterations_valid '{value}'\n"),
                        ("zsh", "MEM_SERVICE_OBJECT_SHARE_ACCESS=cpu-reference\n"
                         "MEM_SERVICE_OBJECT_SHARE_BACKEND=obmm\n"
                         "MEM_SERVICE_OBJECT_SHARE_SIZE=65536\n"
                         "MEM_SERVICE_OBJECT_SHARE_FAULT=none\n"
                         f"MEM_SERVICE_OBJECT_SHARE_ITERATIONS='{value}'\n" + host)):
                    run = subprocess.run([shell, "-s"], input=script,
                                         capture_output=True, text=True, timeout=5)
                    self.assertEqual(run.returncode == 0, valid, run.stderr)

    def test_exact_two_view_and_two_round_evidence(self):
        for rounds in (1, 2):
            result = validator.validate(*fixture(rounds), rounds, 41, 7)
            self.assertEqual(result["status"], "pass")
            self.assertEqual(len(result["views"]), rounds * 2)
            self.assertEqual(result["views"][0]["generation"], 80)

    def test_missing_corrupt_or_reordered_evidence_is_rejected(self):
        for node, old, new in (
            (0, "version=8", "version=7"),
            (0, "generation=80", "generation=81"),
            (0, "reference_hex=", "reference_hex=00"),
            (0, "note=reference_payload_frozen", "note=not_writable"),
            (0, "len=65536", "len=512"),
            (1, "offset=0 len=512 checksum=", "offset=1 len=512 checksum="),
            (1, "base=0x0000700000600080", "base=0x0000700000600081"),
            (1, "cpu_fault_probe=probe_readonly", "cpu_fault_probe=other"),
            (1, "signal=11", "signal=0"),
            (1, "note=out_of_bounds", "note=other"),
            (1, "accessible_bytes=512", "accessible_bytes=65536"),
            (1, "import_handles=1", "import_handles=0"),
            (1, "cleanup_mappings=0", "cleanup_mappings=1"),
            (1, "state=retired", "state=active"),
            (1, "result=ok", "result=failed"),
            (1, "peer_canary=ok", "peer_canary=failed"),
            (1, "action=map_reference", "action=map"),
            (1, "session=client-1", "session=client-0"),
            (1, "op=1 action=", "op=2 action="),
            (1, "phase=drained", "phase=baseline"),
        ):
            with self.subTest(old=old):
                logs = fixture()
                self.assertIn(old, logs[node])
                logs[node] = logs[node].replace(old, new, 1)
                with self.assertRaises((ValueError, KeyError, IndexError)):
                    validator.validate(*logs, 1, 41, 7)
        logs = fixture()
        lines = logs[1].splitlines()
        for index, line in enumerate(lines):
            if "action=" in line or "reference_hex=" in line or "cpu_fault_probe=" in line:
                with self.subTest(missing=line):
                    with self.assertRaises((ValueError, KeyError, IndexError)):
                        validator.validate(logs[0], "\n".join(lines[:index] + lines[index + 1:]), 1, 41, 7)

    def test_cli_writes_summary_only_for_complete_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for node, text in zip(("a", "b"), fixture()):
                (root / node).write_text(text)
            (root / "qemu").write_text(qemu_fixture())
            output = root / "summary.json"
            args = [sys.executable, str(SCRIPT), "--node-a", str(root / "a"),
                    "--node-b", str(root / "b"), "--iterations", "1", "--seed", "41",
                    "--incarnation", "7", "--qemu-b", str(root / "qemu"),
                    "--output", str(output)]
            run = subprocess.run(args, capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(output.read_text())["status"], "pass")
            output.unlink()
            (root / "b").write_text("")
            run = subprocess.run(args, capture_output=True, text=True, timeout=5)
            self.assertNotEqual(run.returncode, 0)
            self.assertFalse(output.exists())

    def test_strict_routes_match_views_and_close_exact_mappings(self):
        for rounds in (1, 2):
            views = validator.validate(*fixture(rounds), rounds, 41, 7)["views"]
            text = qemu_fixture(rounds)
            self.assertEqual(len(validator.validate_qemu(text, views)), 2 * rounds)
            for old, new in (("home_va=0x700000600000", "home_va=0x700000800000"),
                             ("size=0x200000", "size=0x1000"),
                             ("profile=1", "profile=2"), ("source=2", "source=1"),
                             ("epoch=3", "epoch=0"), ("tombstone=no", "tombstone=yes"),
                             ("GSVA_UNMAP: map_id=1", "GSVA_UNMAP: map_id=9")):
                with self.subTest(rounds=rounds, field=old):
                    with self.assertRaises(ValueError):
                        validator.validate_qemu(text.replace(old, new, 1), views)
            for index in range(len(text.splitlines())):
                lines = text.splitlines()
                del lines[index]
                with self.assertRaises(ValueError):
                    validator.validate_qemu("\n".join(lines), views)
            with self.assertRaises(ValueError):
                validator.validate_qemu("GSVA_MAP: obmm bootstrap route\nSIM_DEC: MAP success\n", views)

    def test_generation_comes_from_allocation_receipt(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        helper = "mem_service_object_share_reference_ops() (" + source.split(
            "mem_service_object_share_reference_ops() (", 1)[1].split(
            "\nmem_service_object_share_managed()", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "uptime").write_text("1.0 1.0\n")
            helper = helper.replace("/bin/linqu_mem_service", "reference_cli").replace(
                "/bin/busybox ", "").replace("/proc/uptime", str(root / "uptime")).replace(
                "/tmp/reference-", str(root / "reference-"))
            mock = r'''reference_cli() {
              case "$1" in
                provider-directory-status)
                  printf 'data_plane_ready=0\nprovider_directory_ready=1\n' ;;
                allocate-object) printf 'generation=73\nversion=8\n' ;;
                object-session) cat "$3" ;;
                *) return 2 ;;
              esac
            }
            '''
            run = subprocess.run(["sh", "-s"], input=mock + helper +
                '\nmem_service_object_share_reference 0 65536 1 41 7 unix:fixture\n',
                capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("op=begin_reference key=obj-ref-1 idempotency_key=ref-begin-1 generation=73 version=8", run.stdout)
            self.assertIn("op=seal_reference key=obj-ref-1 idempotency_key=ref-seal-1 generation=73 version=9", run.stdout)
            self.assertEqual(run.stdout.count("op=publish_reference"), 2)
            failed_session = mock.replace('object-session) cat "$3" ;;',
                'object-session) return 1 ;;\n'
                "allocation-stats) printf 'idempotency_capacity=64\\nidempotency_used=59\\n' ;;")
            run = subprocess.run(["sh", "-s"], input=failed_session + helper +
                '\nmem_service_object_share_reference 0 65536 3 41 7 unix:fixture\n',
                capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 1)
            self.assertIn("reference-capacity: node=0 round=1 phase=failed", run.stdout)
            self.assertIn("idempotency_capacity=64", run.stdout)
            self.assertIn("idempotency_used=59", run.stdout)
            self.assertNotIn("key=obj-ref-2", run.stdout)
            not_ready = mock.replace(
                "printf 'data_plane_ready=0\\nprovider_directory_ready=1\\n'",
                "printf 'data_plane_ready=1\\nprovider_directory_ready=0\\n'; "
                f"printf '1000.0 1000.0\\n' > '{root / 'uptime'}'")
            run = subprocess.run(["sh", "-s"], input=not_ready + helper +
                '\nmem_service_object_share_reference 0 65536 1 41 7 unix:fixture\n',
                capture_output=True, text=True, timeout=5)
            self.assertNotEqual(run.returncode, 0)
            self.assertNotIn("reference-allocation:", run.stdout)
            self.assertNotIn("op=begin_reference", run.stdout)
            for receipt in ("generation=0\\nversion=8\\n", "generation=73\\n",
                            "generation=73\\ngeneration=74\\nversion=8\\n",
                            "generation=73\\nversion=8x\\n"):
                bad = mock.replace("generation=73\\nversion=8\\n", receipt)
                run = subprocess.run(["sh", "-s"], input=bad + helper +
                    '\nmem_service_object_share_reference 0 65536 1 41 7 unix:fixture\n',
                    capture_output=True, text=True, timeout=5)
                self.assertNotEqual(run.returncode, 0, receipt)
                self.assertNotIn("op=begin_reference", run.stdout)
            for node in (0, 1):
                run = subprocess.run(["sh", "-s"], input=helper +
                    f'\nmem_service_object_share_reference_ops {node} 65536 1 41 73 8\n',
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(run.returncode, 0, run.stderr)
                actual = [dict(item.split("=", 1) for item in line.split())
                          for line in run.stdout.splitlines()]
                expected = validator.expected_operations(node, "obj-ref-1")
                self.assertEqual(len(actual), len(expected))
                for row, (action, key, status) in zip(actual, expected):
                    self.assertEqual(row["op"], action)
                    self.assertEqual(row.get("expect_status", "ok"), status)
                    if action != "stats":
                        self.assertEqual(row["key"], key)

    @unittest.skipUnless(shutil.which("zsh"), "zsh runner required")
    def test_runner_rejects_invalid_reference_profiles_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / "runner.env"
            for backend, size, rounds, fault in (("loopback", 65536, 1, "none"),
                    ("obmm", 4096, 1, "none"), ("obmm", 65536, 101, "none"),
                    ("obmm", 65536, 1, "sdk_contract")):
                config.write_text(f"MEM_SERVICE_OBJECT_SHARE_ACCESS=cpu-reference\n"
                    f"MEM_SERVICE_OBJECT_SHARE_BACKEND={backend}\nMEM_SERVICE_OBJECT_SHARE_SIZE={size}\n"
                    f"MEM_SERVICE_OBJECT_SHARE_ITERATIONS={rounds}\nMEM_SERVICE_OBJECT_SHARE_FAULT={fault}\n")
                run = subprocess.run(["zsh", str(SCRIPTS / "run_ub_dual_node_apps.sh"),
                    "--config", str(config), "--app", "mem_service_object_share"],
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
                self.assertIn("cpu-reference requires OBMM", run.stderr)


if __name__ == "__main__":
    unittest.main()
