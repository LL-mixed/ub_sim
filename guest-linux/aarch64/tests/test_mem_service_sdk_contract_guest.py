"""Exercise guest contract-op generation and reject incomplete run evidence."""
import importlib.util
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "guest-linux/aarch64/scripts/validate_mem_service_mapping_resources.py"
spec = importlib.util.spec_from_file_location("mapping_contract", SCRIPT)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class ManagedGuestContractTests(unittest.TestCase):
    def test_managed_session_timeouts_fit_cli_contract(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        managed = source.split("mem_service_object_share_managed() (", 1)[1].split(
            "\nrun_mem_service_object_share()", 1)[0]
        cli = (ROOT / "mem_service/apps/mem_service/mem_service.c").read_text()
        limit = int(re.search(r"#define MEM_SERVICE_OBJECT_SESSION_MAX_WAIT_MS (\d+)U", cli)[1])
        timeouts = [int(value) for value in re.findall(r"timeout_ms=(\d+)", managed)]
        self.assertTrue(timeouts)
        self.assertTrue(all(0 < value <= limit for value in timeouts), timeouts)

    def test_capacity_driver_requires_real_exhaustion_before_releasing_barrier(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        helper = "mem_service_object_share_capacity_pressure() (" + source.split(
            "mem_service_object_share_capacity_pressure() (", 1
        )[1].split("\nmem_service_object_share_managed()", 1)[0]
        mock = '''test_cli() {
          case "$1" in
            inspect-allocation) echo state=active ;;
            allocation-stats) echo idempotency_available=0 ;;
            retire-object)
              case "$*" in
                *capacity-barrier-release*) echo barrier_released ;;
                *capacity-pressure-3*) echo "status=$fault_status"; return 1 ;;
                *) echo status=not_found; return 1 ;;
              esac ;;
            *) return 2 ;;
          esac
        }
        '''
        with tempfile.TemporaryDirectory() as directory:
            helper = helper.replace("/bin/linqu_mem_service", "test_cli").replace(
                "/tmp/capacity-", directory + "/capacity-")
            for status, expected in (("capacity_exceeded", 0), ("internal", 1),
                                     ("not_found", 1)):
                result = subprocess.run(
                    ["sh", "-s", "--", status],
                    input='fault_status="$1"\n' + mock + helper +
                          '\nmem_service_object_share_capacity_pressure fixture\n',
                    capture_output=True, text=True, timeout=5,
                )
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertEqual("barrier_released" in result.stdout, expected == 0)
                if expected == 0:
                    self.assertIn("phase=saturated attempts=3", result.stdout)
                    self.assertIn("phase=released status=ok", result.stdout)

    def generated(self, fault, stage, identity="acq-a-1"):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        helper = "mem_service_object_share_contract_ops() (" + source.split(
            "mem_service_object_share_contract_ops() (", 1
        )[1].split("\nmem_service_object_share_managed()", 1)[0]
        result = subprocess.run(
            ["sh", "-s", "--", fault, stage, "obj-share-1", "65536", "255", identity],
            input=helper + '\nmem_service_object_share_contract_ops "$@"\n',
            capture_output=True, text=True, timeout=5,
        )
        return result.returncode, result.stdout.splitlines()

    def test_default_is_unchanged_and_unknown_stage_is_rejected(self):
        for stage in ("acquired", "readonly", "writable", "written", "released"):
            self.assertEqual(self.generated("none", stage), (0, []))
        self.assertEqual(self.generated("sdk_contract", "unknown"), (2, []))

    def test_generated_contract_operations(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        managed = source.split("mem_service_object_share_managed() (", 1)[1].split(
            "\nrun_mem_service_object_share()", 1)[0]
        expected = {
            "acquired": [
                "op=acquire key=absent-obj-share-1 idempotency_key=absent-acq-a-1 expect_status=not_found",
                "op=acquire key=obj-share-1 idempotency_key=stale-acq-a-1 expected_generation=18446744073709551615 expect_status=stale_ref",
                "op=acquire key=obj-share-1 idempotency_key=acq-a-1",
            ],
            "readonly": ["op=write key=obj-share-1 offset=0 len=1 seed=255 expect_status=unsupported"],
            "writable": [
                f"op={action} key=obj-share-1 offset=65536 len=1 seed=255 expect_status=capacity_exceeded"
                for action in ("read", "write", "publish_data", "wait_visible")
            ] + ["op=release key=obj-share-1 idempotency_key=early-acq-a-1 expect_status=internal"],
            "written": ["op=read key=obj-share-1 offset=0 len=1 seed=254 expect_status=checksum_mismatch"],
            "released": ["op=release key=obj-share-1 idempotency_key=acq-a-1"],
        }
        for stage, lines in expected.items():
            with self.subTest(stage=stage):
                self.assertEqual(managed.count(f'contract_ops "$fault" {stage} '), 2)
                self.assertEqual(self.generated("sdk_contract", stage), (0, lines))

    def test_conflict_config_only_probes_with_readonly_mapping(self):
        self.assertEqual(self.generated("map_conflict", "readonly"),
                         (0, ["op=probe_conflict key=obj-share-1"]))
        for stage in ("acquired", "writable", "written", "released"):
            self.assertEqual(self.generated("map_conflict", stage), (0, []))
        self.assertEqual(self.generated("map_conflict", "unknown"), (2, []))

    def descriptor_faults(self):
        return [("descriptor", str(byte)) for byte in
                (0, 4, 8, 12, 16, 24, 32, 40, 44, 48, 56, 64, 68, 72,
                 76, 80, 84, 88, 92, 95)] + [(name, "") for name in
                ("descriptor_length", "descriptor_oversize", "address", "address_len",
                 "size", "alignment", "capabilities", "home", "incarnation")]

    def test_descriptor_config_mutates_acquired_view_before_mapping(self):
        expected = [f"op=map key=obj-share-1 flags=read fault={fault}"
                    + (f" fault_byte={byte}" if byte else "")
                    + " expect_status=stale_ref" for fault, byte in self.descriptor_faults()]
        self.assertEqual(self.generated("descriptor_contract", "acquired"), (0, expected))
        self.assertEqual(len(expected), 29)
        self.assertLessEqual(28 + len(expected), 64)
        self.assertEqual(self.generated("descriptor_contract", "readonly"),
                         (0, ["op=probe_descriptor key=obj-share-1"]))
        for stage in ("writable", "written", "released"):
            self.assertEqual(self.generated("descriptor_contract", stage), (0, []))
        self.assertEqual(self.generated("descriptor_contract", "unknown"), (2, []))

    def descriptor_evidence(self, node):
        lines = []
        prefix = f"mem_service object-session: session=client-{node}"
        for iteration in (1, 2):
            key = f"obj-share-{iteration}"
            for index, (fault, byte) in enumerate(self.descriptor_faults(), 1):
                lines.append(f"{prefix} op={index} action=map key={key} status=stale_ref "
                             f"note=map_failed fault={fault}"
                             + (f" fault_byte={byte}" if byte else ""))
            for index in (31, 35):
                lines.append(f"{prefix} op={index} action=map key={key} status=ok "
                             "base=0x700000000000 len=65536")
                if index == 31:
                    fields = ("magic version length reserved export_id uba size export_token "
                              "home_cna segment epoch segment_flags owner nodes cache access_empty "
                              "access_unknown segment_token_id segment_token_value short_descriptor "
                              "long_descriptor unaligned_uba unaligned_size range_overflow").split()
                    lines.extend(f"obmm-descriptor-probe: field={field} rejected=1 preserved=1"
                                 for field in fields)
                    lines += ["obmm-descriptor-probe: result=pass checks=24 source=retained_handle",
                              f"{prefix} descriptor_probe=pass key={key} checks=24 preserved=1 cleanup_pending=0",
                              f"{prefix} op=33 action=probe_descriptor key={key} status=ok"]
            lines.append(f"{prefix} result=ok ops=57")
        return "\n".join(lines)

    def test_descriptor_validator_rejects_missing_wrong_or_provider_failures(self):
        for node in (0, 1):
            text = self.descriptor_evidence(node)
            validator.validate_descriptor_contract(text, node, 2)
            for bad in (text.replace("status=stale_ref", "status=internal", 1),
                        text.replace("fault_byte=95", "fault_byte=94", 1),
                        text.replace("fault=home", "fault=incarnation", 1),
                        text.replace("note=map_failed", "note=map_cleanup_required", 1),
                        text.replace("result=ok", "result=failed", 1),
                        text.replace("fault_byte=0", "fault_byte=0 fault_byte=0", 1),
                        "\n".join(text.splitlines()[1:]),
                        text + "\n" + text.splitlines()[0],
                        text.replace("action=map", "action=inspect", 1),
                        text.replace("action=probe_descriptor", "action=inspect", 1),
                        text.replace("rejected=1", "rejected=0", 1),
                        text.replace("checks=24", "checks=23", 1),
                        "mem_service obmm-map: result=failed stage=mmap errno=17\n" + text):
                with self.assertRaises(ValueError):
                    validator.validate_descriptor_contract(bad, node, 2)

    def conflict_evidence(self, node):
        lines = []
        for iteration in (1, 2):
            key = f"obj-share-{iteration}"
            base = hex(0x700000000000 + iteration * 0x200000)
            prefix = f"mem_service object-session: session=client-{node}"
            lines += [
                f"{prefix} op=5 action=map key={key} status=ok base={base} len=65536",
                f"mem_service obmm-map: result=failed stage=mmap fixed_va={base} len=65536 errno=17",
                f"{prefix} cpu_conflict_probe=pass key={key} base={base} len=4096 preserved=1 cleanup_pending=0 source=retained_handle",
                f"{prefix} op=3 action=probe_conflict key={key} status=ok",
                f"{prefix} op=9 action=map key={key} status=ok base={base} len=65536",
                f"{prefix} result=ok ops=28",
            ]
        return "\n".join(lines)

    def test_conflict_validator_requires_actual_errno_and_preserved_address(self):
        for node in (0, 1):
            text = self.conflict_evidence(node)
            validator.validate_map_conflict(text, node, 2)
            for bad in (text.replace("errno=17", "errno=12", 1),
                        text.replace("source=retained_handle", "source=sdk_duplicate", 1),
                        text.replace(" source=retained_handle", "", 1),
                        "\n".join(line for line in text.splitlines()
                                  if not line.startswith("mem_service obmm-map:")),
                        text.replace("preserved=1", "preserved=0", 1),
                        text.replace("cleanup_pending=0", "cleanup_pending=1", 1),
                        text.replace("result=ok", "result=failed", 1),
                        text.replace("len=4096", "len=0", 1),
                        text.replace("base=0x700000200000", "base=0x700000300000", 1),
                        text.replace("status=ok base=0x700000200000",
                                     "status=ok base=0x700000300000", 1),
                        text.replace("action=probe_conflict", "action=inspect", 1),
                        "\n".join(text.splitlines()[1:]),
                        text + "\n" + text.splitlines()[0],
                        text.replace("errno=17", "errno=17 errno=17", 1)):
                with self.assertRaises(ValueError):
                    validator.validate_map_conflict(bad, node, 2)

    def evidence(self, node):
        lines = []
        for iteration in (1, 2):
            key = f"obj-share-{iteration}"
            records = [("acquire", "absent-" + key, "not_found", ""),
                       ("acquire", key, "stale_ref", ""),
                       ("write", key, "unsupported", "not_writable"),
                       ("release", key, "internal", "mapping_active"),
                       ("read", key, "checksum_mismatch", "pattern_mismatch")]
            records += [(action, key, "capacity_exceeded", "out_of_bounds")
                        for action in ("read", "write", "publish_data", "wait_visible")]
            records += [(action, key, "ok", "")
                        for action in ("acquire", "acquire", "release", "release")]
            for index, (action, record_key, status, note) in enumerate(records, 1):
                lines.append(f"mem_service object-session: session=client-{node} "
                             f"op={index} action={action} key={record_key} status={status}"
                             + (f" note={note}" if note else ""))
            lines.append(f"mem_service object-session: session=client-{node} result=ok ops=38")
        return "\n".join(lines)

    def test_validator_requires_each_round_rejection_and_replay(self):
        for node in (0, 1):
            text = self.evidence(node)
            validator.validate_sdk_contract(text, node, 2)
            for bad in (text.replace("status=stale_ref", "status=ok", 1),
                        text.replace("note=out_of_bounds", "note=other", 1),
                        text.replace("result=ok", "result=failed", 1),
                        "\n".join(text.splitlines()[1:]),
                        text + "\n" + text.splitlines()[0],
                        text.replace("action=release", "action=inspect", 1),
                        text.replace("action=release key=obj-share-1 status=ok",
                                     "action=inspect key=obj-share-1 status=ok", 1),
                        text.replace("status=stale_ref", "status=stale_ref status=ok", 1),
                        text.replace(f"session=client-{node}", "session=foreign")):
                with self.assertRaises(ValueError):
                    validator.validate_sdk_contract(bad, node, 2)

    def test_cli_requires_contract_in_addition_to_resource_baseline(self):
        fixture_spec = importlib.util.spec_from_file_location(
            "resource_fixture", pathlib.Path(__file__).with_name(
                "test_mem_service_mapping_resources.py"))
        fixture = importlib.util.module_from_spec(fixture_spec)
        fixture_spec.loader.exec_module(fixture)
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "guest.log"
            for node, sequence in ((0, [0, 1, 1, 0, 0]), (1, [0, 1, 1, 0])):
                resources = "".join(fixture.sample(live, node) for live in sequence * 2)
                resources += fixture.kernel_samples()
                for flag, evidence, code in (("--sdk-contract", self.evidence(node), 0),
                                             ("--sdk-contract", "", 1),
                                             ("--map-conflict", self.conflict_evidence(node), 0),
                                             ("--map-conflict", "", 1),
                                             ("--descriptor-contract", self.descriptor_evidence(node), 0),
                                             ("--descriptor-contract", "", 1)):
                    log.write_text(resources + evidence)
                    result = subprocess.run(
                        ["python3", str(SCRIPT), "--log", str(log), "--node", str(node),
                         "--iterations", "2", "--size", "65536", flag],
                        capture_output=True, text=True, timeout=5,
                    )
                    self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                    marker = flag[2:].replace("-", "_")
                    self.assertEqual(f"{marker}=pass node={node}" in result.stdout,
                                     code == 0)


if __name__ == "__main__":
    unittest.main()
