import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "guest-linux/aarch64/scripts"
SCRIPT = SCRIPTS / "validate_managed_pto.py"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("managed_pto_validator", SCRIPT)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
sys.path.pop(0)


def fixture():
    """Synthetic trace parser fixture only; contains no runtime evidence."""
    guests = []
    for node in (0, 1):
        lines = [f"MANAGED_PTO node={node} key=k generation=7 cpu_base=0x700000000000 "
                 "address=0x700000000000 logical_bytes=65536 mapping_id=9 mapping_handle=8"]
        for vector in ([2, 3] if node == 0 else [0, 1, 2, 3]):
            lines.append(f"MANAGED_PTO node={node} path=test vector={vector} "
                         f"version={1 if vector < 2 else vector} "
                         f"checksum={validator.oracle_checksum(vector)} bytes=256 cpu_verify=pass")
        if node == 1:
            for round_, offsets in ((1, "3968,8064,12160"), (2, "12160,8064,16256")):
                lines.append(f"MANAGED_PTO node=1 dispatch={round_} op_id={100+round_} "
                             f"generation=7 mapping_id=9 offsets={offsets} tensor_bytes=256 "
                             "retained_leases=3 release_guard=pass")
                lines.append(f"MANAGED_PTO node=1 dispatch={round_} completion_op={100+round_} "
                             "status=1 error=none")
        lines.append(f"MANAGED_PTO_RESULT node={node} status=pass key=k generation=7 "
                     "mapping_closed=1 holder_released=1 compute_leases=0")
        lines += ["put_object_count=0", "materialize_object_count=0", "live_objects=0"]
        for phase in ("baseline", "drained"):
            lines.append(f"mem_service kernel-regions: phase={phase}")
            lines.append("obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
                         "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0")
        guests.append("\n".join(lines))
    qemu = []
    for request, offsets in ((101, (3968, 8064, 12160)), (102, (12160, 8064, 16256))):
        for binding, offset in enumerate(offsets, 1):
            role = "INPUT" if binding < 3 else "OUTPUT"
            qemu.append(f"QEMU_UB_GM_{role}_AUTHORIZE op={request} request={request} "
                        f"binding={binding} requester_cna=0xf002 local={0x80000000+offset:#x} "
                        "length=256 map=8 generation=9 peer_cna=0x1")
            qemu.append(f"QEMU_UB_GM_GSVA_BIND request={request} binding={binding} segment=0x6 "
                        "home_va=0x700000000000 epoch=7 lease_epoch=8 backing_token=9")
        qemu.append(f"SIM_QEMU_UB_GM_BIND_REGISTER op={request} request={request} "
                    "bindings=3 requester_cna=0xf002")
        for kind, binding in (("LOAD", 1), ("LOAD", 2), ("STORE", 3), ("FENCE", 3)):
            qemu.append(f"QEMU_UB_GM_{kind} request={request} binding={binding} "
                        "addr=0x10000000 length=256 map=8 generation=9")
        qemu.append(f"QEMU_UB_GM_UNBIND op={request} request={request} reason=completion_success "
                    "bindings=3 load_bytes=512 store_bytes=256 fences=1 segment_payload_staging_bytes=0")
    return guests, "\n".join(qemu)


class ManagedPtoValidatorTest(unittest.TestCase):
    def test_complete_parser_fixture(self):
        guests, qemu = fixture()
        result = validator.validate(guests, qemu)
        self.assertEqual(result["status"], "pass")
        self.assertFalse(result["am2_complete"])
        self.assertEqual(result["dispatches"], 2)

    def test_rejects_missing_mismatched_and_duplicate_evidence(self):
        mutations = [
            (0, "cpu_base=0x700000000000", "cpu_base=0x700000010000"),
            (1, "generation=7", "generation=8"),
            (1, "mapping_closed=1", "mapping_closed=0"),
            (1, "compute_leases=0", "compute_leases=1"),
            (0, "put_object_count=0", "put_object_count=1"),
            (0, "materialize_object_count=0", ""),
            (1, "checksum=", "bad_checksum="),
            (1, "status=1 error=none", "status=3 error=timeout"),
            (2, "backing_token=9", "backing_token=10"),
            (2, "home_va=0x700000000000", "home_va=0x700000010000"),
            (2, "local=0x80002f80", "local=0x80003f80"),
            (2, "QEMU_UB_GM_LOAD request=102 binding=1", "MISSING_LOAD request=102 binding=1"),
            (2, "QEMU_UB_GM_FENCE request=102", "MISSING_FENCE request=102"),
            (2, "segment_payload_staging_bytes=0", "segment_payload_staging_bytes=256"),
            (2, "fences=1", "fences=0"),
        ]
        for index, before, after in mutations:
            guests, qemu = fixture()
            pieces = guests + [qemu]
            self.assertIn(before, pieces[index])
            pieces[index] = pieces[index].replace(before, after, 1)
            with self.subTest(mutation=before), self.assertRaises((ValueError, KeyError)):
                validator.validate(pieces[:2], pieces[2])
        guests, qemu = fixture()
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu + "\n" + qemu)

    def test_cli_emits_hashed_report_and_fails_on_empty_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory) / name for name in ("a.log", "b.log", "q.log")]
            guests, qemu = fixture()
            for path, contents in zip(paths, guests + [qemu]):
                path.write_text(contents)
            output = pathlib.Path(directory) / "report.json"
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]), "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(len(json.loads(output.read_text())["sources"]), 3)
            paths[2].write_text("")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(output.read_text())["status"], "fail")


if __name__ == "__main__":
    unittest.main()
