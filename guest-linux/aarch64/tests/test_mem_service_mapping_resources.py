import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/validate_mem_service_mapping_resources.py"
spec = importlib.util.spec_from_file_location("mapping_resources", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample(live, node=1):
    return (
        f"mem_service object-session: action=stats status=ok import_mappings={live}\n"
        "mem_service provider-resources: provider=obmm scope=endpoint version=1 "
        f"export_handles=1 export_bytes=2097152 import_handles={live * node} "
        f"import_bytes={live * node * 2097152} vma_count={live * 2} "
        f"vma_bytes={live * 2097152} accessible_views={live} "
        f"accessible_bytes={live * 65536} cleanup_mappings=0 closing=0 "
        "control_close_uncertain=0\n"
    )


def kernel_samples():
    return "".join(
        f"mem_service kernel-regions: phase={phase}\n"
        "obmm-kernel-region: id=1 type=export bytes=2097152\n"
        "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
        "export_handles=1 export_bytes=2097152 import_handles=0 import_bytes=0\n"
        for phase in ("baseline", "drained")
    )


def allocation_pressure_sample(size=65536):
    # Deliberately allocate clients 3, 2, 1 in that order.
    geometry = {
        "obj-share-1": (1, size, 0, 0), "obj-sent-1": (2, 65536, 0, 2),
        "obj-returned-1": (3, 65536, 0, 4),
        "allocation-1": (6, 1, 2097152, 20),
        "allocation-2": (5, 2097153, 4194304, 16),
        "allocation-3": (4, 4194305, 8388608, 8),
        "allocation-4": (7, 3145728, 4194304, 24),
    }

    def view(key, active):
        generation, logical, alignment, offset = geometry[key]
        address = 0x700000000000 + offset * 1048576
        length = (logical + 2097151) // 2097152 * 2097152
        segment, epoch, export = 0xc4c2000000000000 + generation, 1, generation + 90
        descriptor = bytearray(96)
        for position, value in ((16, export), (24, address), (32, length),
                                (48, segment), (56, epoch)):
            descriptor[position:position + 8] = value.to_bytes(8, "big")
        fields = {"key": key, "generation": generation, "size_bytes": logical,
                  "alignment_bytes": alignment, "home_node": "guest-node-0",
                  "provider_incarnation": 7, "address": address if active else 0,
                  "address_len": length if active else 0, "provider_backed": int(active),
                  "live_refs": 0, "state": "active" if active else "retired",
                  "descriptor_len": 96 if active else 0,
                  "descriptor_hex": descriptor.hex() if active else ""}
        result = "mem_service inspect-allocation: status=ok\n" + "".join(
            f"{name}={value}\n" for name, value in fields.items())
        if active:
            result += (f"mem_service allocation-segment: key={key} segment={segment:#x} "
                       f"address={address:#x} size={length:#x} epoch={epoch:#x}\n")
        return result

    original = tuple(geometry)[:3]
    added = tuple(geometry)[3:]
    text = ""
    for phase, live, backing, keys, active in (
            ("before", 3, 6, original, True), ("live", 6, 18, added[:3], True),
            ("hole", 5, 14, (added[1],), False), ("replaced", 6, 18, (added[3],), True),
            ("drained", 3, 6, added, False)):
        text += f"mem_service allocation-pressure: phase={phase}\n"
        stats = {"backing_allocated_bytes": backing * 1048576,
                 "address_reserved_bytes": backing * 1048576, "live_objects": live,
                 "export_mappings": live, "import_mappings": 2, "live_refs": 2,
                 "in_flight": 0, "quarantined_objects": 0, "quarantined_bytes": 0,
                 "idempotency_reservation_deficit": 0}
        text += "mem_service allocation-stats: status=ok\n" + "".join(
            f"{key}={value}\n" for key, value in stats.items())
        text += (f"mem_service allocation-pool: phase={phase} used={(backing + 2) * 1048576} "
                 "consistency=non_atomic\n")
        text += "".join(view(key, active) for key in keys)
        if phase == "live":
            for index in range(1, 4):
                text += f"mem_service allocation-client: index={index} pid={100 + index} phase=waiting\n"
        if phase == "drained":
            for index, key in enumerate(added[:3], 1):
                generation = geometry[key][0]
                for op, (action, state) in enumerate((("allocate", "allocating"),
                        ("wait_state", "active"), ("allocate", "allocating"), ("wait_state", "retired")), 1):
                    text += (f"mem_service object-session: session=allocation-client-{index} "
                             f"op={op} action={action} key={key} status=ok state={state} generation={generation}\n")
                text += f"mem_service object-session: session=allocation-client-{index} result=ok ops=4\n"
                text += f"mem_service allocation-client: index={index} pid={100 + index} phase=exited code=0\n"
    text += "mem_service allocation-pressure: phase=complete\n"
    for key in added:
        generation = geometry[key][0]
        for phase in ("reserve-intent", "reserved", "exported", "published",
                      "release-intent", "unexported", "retired", "reclaimed"):
            segment = 0 if phase == "reserve-intent" else 0xc4c2000000000000 + generation
            export = generation + 90 if phase in ("exported", "published", "release-intent", "unexported") else 0
            text += f"obmm-worker generation={generation} phase={phase} segment={segment} export={export}\n"
    return text


class MappingResourceValidationTests(unittest.TestCase):
    def test_failure_snapshot_is_bounded_and_preserves_query_errors(self):
        runner = (SCRIPT.parents[1] / "initramfs/run_app").read_text()
        helper = "mem_service_object_share_failure_snapshot() (" + runner.split(
            "mem_service_object_share_failure_snapshot() (", 1)[1].split(
            "\nmem_service_object_share_managed() (", 1)[0]
        self.assertNotIn("inspect-allocation", helper)
        self.assertNotIn("reclaim", helper)
        for code in (0, 7, 124):
            with self.subTest(code=code):
                mock = ("fake_busybox() { [ \"$1\" = timeout ] && "
                        "[ \"$2\" = 5 ] || return 99; "
                        "printf 'invoked: %s\\n' \"$*\"; return " + str(code) + "; }\n")
                script = mock + helper.replace("/bin/busybox", "fake_busybox")
                script += "\nmem_service_object_share_failure_snapshot tcp:10.0.0.1:12347 0 failed\n"
                result = subprocess.run(["sh", "-c", script], capture_output=True,
                                        text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.count("invoked:"), 3)
                for operation in ("allocation-stats", "provider-directory-status",
                                  "kernel-regions"):
                    self.assertIn(f"operation={operation} exit_code={code}", result.stdout)
                self.assertIn("consistency=non_atomic", result.stdout)

    def test_failure_cleanup_snapshots_bracket_worker_and_keep_client_exit(self):
        runner = (SCRIPT.parents[1] / "initramfs/run_app").read_text()
        managed = runner.split("mem_service_object_share_managed() (", 1)[1]
        cleanup = "cleanup_managed_share() {" + managed.split(
            "cleanup_managed_share() {", 1)[1].split(
            "\n  trap cleanup_managed_share EXIT", 1)[0]
        mocks = """
connect=tcp:10.0.0.1:12347
node=0
capacity_pid=
worker_pid=42
daemon_pid=43
mem_service_object_share_failure_snapshot() { echo "snapshot:$3"; }
kill() { echo "kill:$*"; }
wait() { echo "wait:$*"; }
"""
        for code in (0, 1, 37, 135):
            with self.subTest(code=code):
                result = subprocess.run(
                    ["sh", "-c", mocks + cleanup +
                     "\ntrap cleanup_managed_share EXIT\nexit " + str(code)],
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
                if code:
                    lines = result.stdout.splitlines()
                    self.assertLess(lines.index("snapshot:before-worker-stop"),
                                    lines.index("kill:42"))
                    self.assertLess(lines.index("wait:42"),
                                    lines.index("snapshot:after-worker-stop"))
                    self.assertLess(lines.index("snapshot:after-worker-stop"),
                                    lines.index("kill:43"))
                else:
                    self.assertNotIn("snapshot:", result.stdout)

    def test_provider_descriptor_checks_are_complete_and_preserve_mapping(self):
        fields = ("magic version length reserved export_id uba size export_token home_cna "
                  "segment epoch segment_flags owner nodes cache access_empty access_unknown "
                  "segment_token_id segment_token_value short_descriptor long_descriptor "
                  "unaligned_uba unaligned_size range_overflow").split()
        checks = "".join(f"obmm-descriptor-probe: field={field} rejected=1 preserved=1\n"
                         for field in fields)
        done = "obmm-descriptor-probe: result=pass checks=24 source=retained_handle\n"
        session = ("mem_service object-session: session=client-1 descriptor_probe=pass "
                   "key=obj-share-1 checks=24 preserved=1 cleanup_pending=0\n")
        good = checks + done + session
        module.validate_provider_descriptor(good, 1, 1)
        for bad in (good.replace("field=epoch", "field=segment"),
                    good.replace("checks=24", "checks=23", 1),
                    good.replace("rejected=1", "rejected=0", 1),
                    good.replace("cleanup_pending=0", "cleanup_pending=1"),
                    good.replace("session=client-1", "session=client-0"),
                    good.replace("field=epoch", "field=epoch field=epoch"),
                    good.replace("cleanup_pending=0", "cleanup_pending=0 cleanup_pending=0"),
                    good.replace(done, ""), good + session, checks + session + done):
            with self.assertRaises(ValueError):
                module.validate_provider_descriptor(bad, 1, 1)

    def test_allocation_pressure_checks_geometry_identity_replay_and_drain(self):
        text = allocation_pressure_sample()
        module.validate_allocation_pressure(text, 0, 1, 65536)
        for bad in ("", text + text,
                    text.replace("phase=live", "phase=hole", 1),
                    text.replace("import_mappings=2", "import_mappings=1"),
                    text.replace("used=20971520", "used=16777216", 1),
                    text.replace("alignment_bytes=8388608", "alignment_bytes=2097152", 1),
                    text.replace("address_len=6291456", "address_len=4194304", 1),
                    text.replace("home_node=guest-node-0", "home_node=guest-node-1", 1),
                    text.replace("segment=0xc4c2000000000006", "segment=0xc4c2000000000005", 1),
                    text.replace("epoch=0x1", "epoch=0x2", 1),
                    text.replace("phase=unexported", "phase=release-unknown", 1),
                    text.replace("phase=reserved", "phase=reserve-intent", 1),
                    text.replace("export=96", "export=95", 1),
                    text.replace("phase=waiting", "phase=exited", 1),
                    text.replace("pid=102", "pid=101"),
                    text.replace("code=0", "code=1", 1),
                    text.replace("result=ok ops=4", "result=ok ops=3", 1),
                    text.replace("op=3 action=allocate", "op=3 action=inspect", 1),
                    text.replace("state=allocating generation=6", "state=allocating generation=7", 1),
                    text.replace("consistency=non_atomic", "consistency=non_atomic used=1", 1),
                    text.replace("provider_backed=0", "provider_backed=1", 1)):
            with self.subTest(bad=bad), self.assertRaises((ValueError, KeyError)):
                module.validate_allocation_pressure(bad, 0, 1, 65536)
        with self.assertRaises(ValueError):
            module.validate_allocation_pressure(text, 0, 2, 65536)
        module.validate_allocation_pressure(
            "action=wait_state key=obj-returned-1 status=ok state=retired", 1, 1, 65536)

    def test_allocation_pressure_cli_and_guest_parser(self):
        runner = (SCRIPT.parents[1] / "initramfs/run_app").read_text()
        helper = runner.split("mem_service_object_share_allocation_snapshot() (", 1)[1].split(
            "\nmem_service_object_share_allocation_pressure() (", 1)[0]
        expression = helper.split('/bin/busybox sed -n "', 1)[1].split('"', 1)[0]
        expression = expression.replace("\\\\", "\\").replace("$key", "allocation-2")
        query = ("[gva_manager] gsva descriptor action=query version=1 flags=0 "
                 "segment_id=0xc4c2000000000005 home_va=0x700001000000 size=0x400000 "
                 "epoch=0x1 home_cna=42 token_id=9 token_value=12345\n")
        parsed = subprocess.run(["sed", "-n", expression], input=query,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(parsed.returncode, 0, parsed.stderr)
        self.assertEqual(parsed.stdout, "mem_service allocation-segment: key=allocation-2 "
                         "segment=0xc4c2000000000005 address=0x700001000000 size=0x400000 epoch=0x1\n")
        pressure = runner.split("mem_service_object_share_allocation_pressure() (", 1)[1].split(
            "\nmem_service_object_share_managed() (", 1)[0]
        for required in ("trap cleanup_allocation_pressure EXIT", "kill -0", 'wait "$pid"',
                         "allocation-pressure-hole.conf", "alignment_bytes=4194304"):
            self.assertIn(required, pressure)
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "guest.log"
            log.write_text("".join(sample(x, 0) for x in (0, 1, 1, 0, 0)) +
                           kernel_samples() + allocation_pressure_sample())
            result = subprocess.run(["python3", str(SCRIPT), "--log", str(log), "--node", "0",
                                     "--iterations", "1", "--size", "65536", "--allocation-pressure"],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("allocation_pressure=pass node=0", result.stdout)

    def test_backing_query_parser_uses_bundled_busybox(self):
        runner = SCRIPT.parents[1] / "initramfs/run_app"
        helper = runner.read_text().split(
            "mem_service_object_share_backing_pressure() (", 1)[1].split(
                "\nmem_service_object_share_managed() (", 1)[0]
        self.assertNotIn("awk", helper)
        query = ("gva_manager: result=done action=gsva-segment-query "
                 "segment_id=0xc4c2000000000001 home_va=0x700000000000 epoch=0x1\n")
        for field, source, expected in (
                ("address", "key=obj-share-1\naddress=0x700000000000\n", "0x700000000000\n"),
                ("segment", query, "0xc4c2000000000001\n"),
                ("epoch", query, "0x1\n")):
            marker = f"{field}=$(/bin/busybox sed -n '"
            self.assertIn(marker, helper)
            expression = helper.split(marker, 1)[1].split("'", 1)[0]
            for text, wanted in ((source, expected), ("unrelated diagnostic\n", "")):
                with self.subTest(field=field, text=text):
                    result = subprocess.run(["sed", "-n", expression], input=text,
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, wanted)

    def test_backing_capacity_requires_kernel_receipt_and_unchanged_pool_usage(self):
        text = "obmm-worker generation=1 phase=published segment=73 export=91\n"
        text += "mem_service backing-pin: segment=0x49 epoch=0x1 errno=16\n"
        fields = ("version", "flags", "reserved") + tuple("segment." + name for name in
            ("version", "flags", "segment_id", "home_va", "size", "epoch", "home_cna",
             "owner_node_id", "node_count", "cache_policy", "p_tag", "access_flags",
             "token_id", "token_value"))
        for field in fields:
            error = 116 if field.startswith("segment.") else 22
            text += f"[gva_manager] export_identity field={field} errno={error} preserved=1 segment=0x49\n"
        text += "[gva_manager] result=done action=gsva-export-identity-check checks=17 segment=0x49\n"
        for phase, live in (("before", 3), ("rejected", 3), ("recovered", 4), ("drained", 3)):
            text += f"mem_service backing-pressure: phase={phase}\n"
            fields = {"backing_allocated_bytes": live * 2097152,
                      "address_reserved_bytes": live * 2097152, "live_objects": live,
                      "export_mappings": live, "import_mappings": 2, "live_refs": 2,
                      "in_flight": 0, "quarantined_objects": 0, "quarantined_bytes": 0,
                      "idempotency_reservation_deficit": 0}
            text += "mem_service allocation-stats: status=ok\n" + "".join(
                f"{key}={value}\n" for key, value in fields.items())
            text += (f"mem_service backing-pool: phase={phase} total=1073741824 "
                     f"used={live * 2097152 + 4194304} ready=1048576000 consistency=non_atomic\n")
            if phase in ("rejected", "recovered"):
                active = phase == "recovered"
                fields = {"key": "backing-recovered" if active else "backing-empty",
                          "generation": 5 if active else 4, "state": "active" if active else "retired",
                          "size_bytes": 65536 if active else 2147483648,
                          "address": 0x700080600000 if active else 0,
                          "address_len": 2097152 if active else 0,
                          "provider_backed": int(active), "live_refs": 0,
                          "descriptor_len": 96 if active else 0,
                          "descriptor_hex": "ab" * 96 if active else ""}
                text += "mem_service inspect-allocation: status=ok\n" + "".join(
                    f"{key}={value}\n" for key, value in fields.items())
        text += "mem_service backing-pressure: phase=complete\n"
        for phase in ("reserve-intent", "reserved", "export-no-backing", "unbacked-retire-intent",
                      "unbacked-retired", "capacity-reject-intent", "cancel-empty", "cancel-confirmed"):
            segment = 74 if phase in ("reserved", "export-no-backing", "unbacked-retire-intent",
                                      "unbacked-retired") else 0
            text += f"obmm-worker generation=4 phase={phase} segment={segment} export=0\n"
        text += "obmm-worker generation=4 state=retired reason=backing_allocation size=2147483648 alignment=0\n"
        module.validate_backing_capacity(text, 0, 1)
        for bad in ("", text + text, text.replace("errno=16", "errno=22"),
                    text.replace("field=segment.epoch", "field=segment.size"),
                    text.replace("errno=116", "errno=22", 1),
                    text.replace("preserved=1", "preserved=0", 1),
                    text.replace("checks=17", "checks=16"),
                    text.replace("segment=0x49", "segment=0x50"),
                    text.replace("phase=export-no-backing", "phase=export-unknown"),
                    text.replace("phase=unbacked-retired", "phase=retire-unknown"),
                    text.replace("segment=74", "segment=75", 1),
                    text.replace("segment=74 export=0", "segment=74 export=91", 1),
                    text.replace("reason=backing_allocation", "reason=address_capacity"),
                    text.replace("used=10485760", "used=12582912", 1),
                    text.replace("total=1073741824", "total=2147483648"),
                    text.replace("consistency=non_atomic", "consistency=atomic"),
                    text.replace("import_mappings=2", "import_mappings=1"),
                    text.replace("state=retired", "state=allocating"),
                    text.replace(str(0x700080600000), str(0x700000600000)),
                    text.replace("generation=5", "generation=4"),
                    text.replace("phase=complete", "phase=drained")):
            with self.subTest(bad=bad), self.assertRaises((ValueError, KeyError)):
                module.validate_backing_capacity(bad, 0, 1)
        with self.assertRaises(ValueError):
            module.validate_backing_capacity(text, 0, 2)
        module.validate_backing_capacity(
            "action=wait_state key=obj-returned-1 status=ok state=retired", 1, 1)

    def test_address_capacity_requires_actual_geometry_and_preserved_mappings(self):
        def view(key, generation, size, alignment=0, address=0, length=0):
            fields = {"key": key, "generation": generation, "size_bytes": size,
                      "alignment_bytes": alignment, "address": address, "address_len": length,
                      "provider_backed": int(bool(address)), "live_refs": 0,
                      "state": "active" if address else "retired",
                      "descriptor_len": 96 if address else 0,
                      "descriptor_hex": f"{generation:02x}" * 96 if address else ""}
            return "mem_service inspect-allocation: status=ok\n" + "".join(
                f"{key}={value}\n" for key, value in fields.items())

        def stats(backing, live):
            fields = {"backing_allocated_bytes": backing, "address_reserved_bytes": backing,
                      "live_objects": live, "export_mappings": live, "import_mappings": 2,
                      "live_refs": 2, "in_flight": 0, "quarantined_objects": 0,
                      "quarantined_bytes": 0, "idempotency_reservation_deficit": 0}
            return "mem_service allocation-stats: status=ok\n" + "".join(
                f"{key}={value}\n" for key, value in fields.items())

        base = 0x700000000000
        text = "mem_service address-pressure: phase=filled\n"
        for key, generation, size, alignment, offset, length in (
                ("obj-share-1", 1, 65537, 4194304, 0, 2097152),
                ("obj-sent-1", 2, 65536, 0, 2097152, 2097152),
                ("obj-returned-1", 3, 65536, 0, 4194304, 2097152),
                ("address-fill", 6, 8388608, 8388608, 8388608, 8388608),
                ("address-gap", 7, 2097152, 0, 6291456, 2097152)):
            text += view(key, generation, size, alignment, base + offset, length)
        text += view("address-overflow", 4, 2**64 - 1)
        text += view("address-alignment", 5, 1, 2**63)
        text += stats(16777216, 5)
        text += "mem_service address-pressure: phase=reused\n"
        text += view("address-fill", 6, 8388608, 8388608)
        text += view("address-exhausted", 8, 1)
        text += view("address-reused", 9, 8388608, 8388608, base + 8388608, 8388608)
        text += stats(16777216, 5)
        text += "mem_service address-pressure: phase=rejected\n"
        text += view("address-gap", 7, 2097152)
        text += view("address-reused", 9, 8388608, 8388608)
        text += stats(6291456, 3)
        text += "mem_service address-pressure: phase=released\n"
        for generation in (4, 5, 8):
            text += f"obmm-worker generation={generation} state=retired reason=address_capacity\n"
        for phase in ("reserve-intent", "reserve-empty", "capacity-reject-intent",
                      "cancel-empty", "cancel-confirmed"):
            text += f"obmm-worker generation=8 phase={phase} segment=0 export=0\n"
        module.validate_address_capacity(text, 0, 1, 65537)
        for bad in ("", text + text,
                    text.replace("phase=released", "phase=filled"),
                    text.replace("address_len=8388608", "address_len=4194304"),
                    text.replace(f"address={base + 8388608}", f"address={base}"),
                    text.replace("alignment_bytes=4194304", "alignment_bytes=2097152"),
                    text.replace("state=retired", "state=allocating", 1),
                    text.replace("import_mappings=2", "import_mappings=1"),
                    text.replace("backing_allocated_bytes=6291456", "backing_allocated_bytes=14680064"),
                    text.replace("reason=address_capacity", "reason=unknown", 1),
                    text.replace("phase=reserve-empty", "phase=reserve-unknown"),
                    text.replace("phase=reserve-empty segment=0", "phase=reserve-empty segment=1"),
                    text.replace("descriptor_hex=" + "09" * 96, "descriptor_hex=" + "06" * 96),
                    text.replace("generation=9", "generation=6"),
                    text + "obmm-worker generation=4 phase=reserve-intent segment=0 export=0\n",
                    text.replace("key=address-exhausted", "key=address-no-reuse")):
            with self.subTest(bad=bad), self.assertRaises((ValueError, KeyError)):
                module.validate_address_capacity(bad, 0, 1, 65537)
        with self.assertRaises(ValueError):
            module.validate_address_capacity(text, 0, 2, 65537)
        module.validate_address_capacity(
            "action=wait_state key=obj-returned-1 status=ok state=retired", 1, 1, 65537)

    def test_capacity_requires_saturation_then_complete_resource_drain(self):
        def capacity_stats(drained):
            fields = {
                "import_mappings": 0 if drained else 2,
                "live_refs": 0 if drained else 2,
                "live_objects": 0 if drained else 3,
                "in_flight": 0,
                "backing_allocated_bytes": 0 if drained else 6291456,
                "address_reserved_bytes": 0 if drained else 6291456,
                "idempotency_capacity": 64,
                "idempotency_used": 64 if drained else 55,
                "idempotency_cleanup_reserved": 0 if drained else 9,
                "idempotency_available": 0,
                "idempotency_reservation_deficit": 0,
            }
            return "mem_service allocation-stats: status=ok\n" + "".join(
                f"{key}={value}\n" for key, value in fields.items())
        valid = ("mem_service capacity-pressure: phase=saturated attempts=40\n" +
                 capacity_stats(False) +
                 "mem_service capacity-pressure: phase=released status=ok\n" +
                 capacity_stats(True))
        module.validate_capacity(valid, 0, 1)
        for text in ("", valid + valid,
                     valid.replace("import_mappings=2", "import_mappings=1"),
                     valid.replace("idempotency_used=55", "idempotency_used=54"),
                     valid.replace("idempotency_available=0", "idempotency_available=1"),
                     valid.replace("idempotency_reservation_deficit=0", "idempotency_reservation_deficit=1"),
                     valid.replace("backing_allocated_bytes=0", "backing_allocated_bytes=4096"),
                     valid.replace("idempotency_used=64", "idempotency_used=63"),
                     valid.replace("idempotency_cleanup_reserved=9", "idempotency_cleanup_reserved=8")):
            with self.subTest(text=text), self.assertRaises((ValueError, KeyError)):
                module.validate_capacity(text, 0, 1)
        peer = "action=wait_state key=obj-returned-1 status=ok state=retired"
        module.validate_capacity(peer, 1, 1)
        for text, iterations in (("", 1), (peer, 2)):
            with self.assertRaises(ValueError):
                module.validate_capacity(text, 1, iterations)

    def test_both_nodes_have_baseline_live_and_drained_resources(self):
        for node, sequence in ((0, [0, 1, 1, 0, 0]), (1, [0, 1, 1, 0])):
            module.validate("".join(sample(live, node) for live in sequence * 2), node, 2, 65536)
        with self.assertRaises(ValueError):
            module.validate("".join(sample(live) for live in [0, 1, 1, 0, 0]), 0, 1, 65536)

    def test_missing_fabricated_leaked_and_reordered_samples_fail(self):
        valid = "".join(sample(live) for live in [0, 1, 1, 0])
        for text in (
            "", valid.replace("import_mappings=1", "import_mappings=0"),
            valid.replace("cleanup_mappings=0", "cleanup_mappings=1"),
            valid.replace("vma_bytes=2097152", "vma_bytes=65536"),
            valid.replace("import_bytes=2097152", "import_bytes=0"),
            "".join(sample(live) for live in [0, 1, 0, 1]),
            valid + sample(0), valid.replace("version=1", "version=2"),
        ):
            with self.subTest(text=text), self.assertRaises((ValueError, KeyError)):
                module.validate(text, 1, 1, 65536)

    def test_cli_validates_complete_log_and_rejects_empty_log(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "guest.log"
            for text, code in (("".join(sample(x) for x in [0, 1, 1, 0]) + kernel_samples(), 0), ("", 1)):
                log.write_text(text)
                result = subprocess.run(
                    ["python3", str(SCRIPT), "--log", str(log), "--node", "1",
                     "--iterations", "1", "--size", "65536"],
                    text=True, capture_output=True, timeout=10,
                )
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)

    def test_kernel_census_requires_identical_region_ids_types_and_sizes(self):
        valid = kernel_samples()
        module.validate_kernel_regions(valid)
        for text in ("", valid + valid, valid.replace("phase=drained", "phase=baseline"),
                     valid.replace("id=1", "id=2", 1),
                     valid.replace("export_bytes=2097152", "export_bytes=0"),
                     valid.replace("type=export", "type=unknown"),
                     valid.replace("consistency=non_atomic", "consistency=atomic"),
                     valid.replace("obmm-kernel-region: id=1 type=export bytes=2097152\n", "", 1),
                     valid.replace("obmm-kernel-region: id=1", "obmm-kernel-region: id=-1")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                module.validate_kernel_regions(text)
