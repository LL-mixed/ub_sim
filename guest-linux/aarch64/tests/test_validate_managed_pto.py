import importlib.util
import hashlib
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
                        f"addr={0x80000000+offsets[binding-1]:#x} length=256 map=8 generation=9")
        qemu.append(f"QEMU_UB_GM_UNBIND op={request} request={request} reason=completion_success "
                    "bindings=3 load_bytes=512 store_bytes=256 fences=1 segment_payload_staging_bytes=0")
    return guests, "\n".join(qemu)


def owned_fixture(recovery=False, threads=4):
    guests, qemu = fixture()
    for node in (0, 1):
        first, rest = guests[node].split("\n", 1)
        adopted = f"MANAGED_OWNER node={node} state=adopted generation=7 mapping_id=9 "
        guests[node] = first + "\n" + adopted + "cpu_pins=1 holder_transferred=1\n" + rest
        marker = f"MANAGED_PTO_RESULT node={node} "
        drained = f"MANAGED_OWNER node={node} state=drained cpu_pins=0 compute_pins=0 "
        guests[node] = guests[node].replace(marker,
            drained + "in_flight_pins=0 mapping_pending=0 holder_pending=0\n" + marker)
    if recovery:
        for round_ in (1, 2):
            marker = f"MANAGED_PTO node=1 dispatch={round_} completion_op="
            waiting = f"MANAGED_PTO_WAIT node=1 dispatch={round_} op_id={100+round_} "
            guests[1] = guests[1].replace(marker, waiting + "state=timeout retained_leases=3 "
                "close_guard=pass release_guard=pass\n" + waiting +
                f"state=recovered completion_op={100+round_} pending_op=0\n" + marker)
    compute = 3 if recovery else 0
    closing = f"MANAGED_OWNER node=1 state=closing cpu_pins={threads+1} compute_pins={compute} "
    closing += f"in_flight_pins={compute} close_guard=pass admission_guard=pass threads={threads} "
    closing += f"operation_id={102 if recovery else 0}\n"
    marker = "MANAGED_PTO_WAIT node=1 dispatch=2 op_id=102 state=timeout" if recovery else (
        "MANAGED_OWNER node=1 state=drained")
    guests[1] = guests[1].replace(marker, closing + marker)
    marker = "MANAGED_OWNER node=1 state=drained"
    workers = f"MANAGED_OWNER node=1 state=workers-drained threads={threads} checks={threads*10} "
    workers += f"after_seal_threads={threads} cpu_verify=pass\n"
    guests[1] = guests[1].replace(marker, workers + marker)
    return guests, qemu


def negative_fixture():
    guests, qemu = owned_fixture()
    rows, failures = [], []
    for index, name in enumerate(("bad-mapping-ref", "wrong-requester", "oob", "role-access-mismatch",
                                  "tstore-on-read", "tload-on-write")):
        operation = 103 + index
        error = "pto_ub_gm_access_denied" if index in (1, 4, 5) else "pto_ub_gm_bad_memref"
        rows += [f"MANAGED_PTO_NEGATIVE node=1 case={name} state=submitted op_id={operation} "
                 f"mapping_id=9 expected={error} metadata_crc=0x12345678 retained_leases=3",
                 f"MANAGED_PTO_NEGATIVE node=1 case={name} state=rejected op_id={operation} "
                 f"completion_op={operation} status=3 error={error} pending_op=0"]
        for vector in range(4):
            checksum = validator.oracle_checksum(vector) if vector < 2 else "3625c1e461d4b383"
            rows.append(f"MANAGED_PTO_NEGATIVE_DATA node=1 case={name} op_id={operation} "
                        f"vector={vector} checksum={checksum} bytes=256 unchanged=pass")
        if index < 4:
            failures += [f"QEMU_UB_GM_FAILURE_COMPLETION op={operation} cq_slot={index} "
                         f"cq_tail={index+1} status=3 code={error}",
                         f"QEMU_UB_GM_DISPATCH_REJECT op={operation} error=3 code={error}"]
        else:
            execution = qemu.split("QEMU_UB_GM_INPUT_AUTHORIZE op=102")[0]
            execution = execution.replace("op=101", f"op={operation}").replace(
                "request=101", f"request={operation}")
            execution = execution.replace("completion_success", "completion_failure").replace(
                "store_bytes=256 fences=1", "store_bytes=0 fences=0")
            excluded = ("QEMU_UB_GM_STORE ", "QEMU_UB_GM_FENCE ")
            failures += [line for line in execution.splitlines() if not line.startswith(excluded)]
    marker = "MANAGED_PTO node=1 dispatch=1 op_id="
    guests[1] = guests[1].replace(marker, "\n".join(rows) + "\n" + marker)
    return guests, "\n".join(failures) + "\n" + qemu


class ManagedPtoValidatorTest(unittest.TestCase):
    def test_execution_negatives_require_runtime_registration_and_zero_writes(self):
        guests, qemu = negative_fixture()
        mutations = (
            ("OUTPUT_AUTHORIZE op=107 request=107 binding=3", "INPUT_AUTHORIZE op=107 request=107 binding=3"),
            ("INPUT_AUTHORIZE op=108 request=108 binding=1", "OUTPUT_AUTHORIZE op=108 request=108 binding=1"),
            ("op=107 request=107 reason=completion_failure", "op=107 request=107 reason=bridge_submit_failed"),
            ("load_bytes=512 store_bytes=0", "load_bytes=256 store_bytes=0"),
            ("load_bytes=512 store_bytes=0", "load_bytes=0 store_bytes=0"),
            ("store_bytes=0 fences=0", "store_bytes=256 fences=1"),
            ("fences=0 segment_payload_staging_bytes=0", "fences=0 segment_payload_staging_bytes=256"),
            ("GSVA_BIND request=107 binding=1 segment=0x6", "GSVA_BIND request=107 binding=1 segment=0x7"),
            ("LOAD request=107 binding=1 addr=0x80000f80", "LOAD request=107 binding=1 addr=0x80000f81"),
        )
        for before, after in mutations:
            self.assertIn(before, qemu)
            with self.subTest(before=before), self.assertRaises(ValueError):
                validator.validate(guests, qemu.replace(before, after),
                                   access_threads=4, require_negative_checks=True)
        lines = qemu.splitlines()
        for prefix in ("SIM_QEMU_UB_GM_BIND_REGISTER op=107 ", "QEMU_UB_GM_UNBIND op=108 "):
            line = next(row for row in lines if row.startswith(prefix))
            for changed in (qemu.replace(line, ""), qemu + "\n" + line):
                with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                    validator.validate(guests, changed, access_threads=4, require_negative_checks=True)
        for extra in ("QEMU_UB_GM_STORE request=107 binding=3 addr=0x80002f80 length=256 map=8 generation=9",
                      "QEMU_UB_GM_LOAD request=108 binding=1 addr=0x80000f80 length=256 map=8 generation=9",
                      "QEMU_UB_GM_FAILURE_COMPLETION op=107 status=3 code=pto_ub_gm_access_denied"):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                validator.validate(guests, qemu + "\n" + extra,
                                   access_threads=4, require_negative_checks=True)
        late = [row for row in lines if "request=107 " in row]
        changed = [row for row in lines if row not in late] + late
        with self.assertRaises(ValueError):
            validator.validate(guests, "\n".join(changed), access_threads=4, require_negative_checks=True)

    def test_negative_probes_require_exact_failures_unchanged_data_and_recovery(self):
        guests, qemu = negative_fixture()
        result = validator.validate(guests, qemu, access_threads=4, require_negative_checks=True)
        self.assertEqual((result["dispatches"], result["successful_dispatches"],
                          result["expected_rejections"]), (8, 2, 6))
        self.assertEqual((result["execution_rejections"], result["load_callbacks"]), (2, 8))
        self.assertFalse(result["am2_complete"])
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu, access_threads=4)
        with self.assertRaises(ValueError):
            validator.validate(*owned_fixture(), access_threads=4, require_negative_checks=True)
        for before, after in (("completion_op=103 status=3", "completion_op=999 status=3"),
                              ("pending_op=0", "pending_op=103"),
                              ("3625c1e461d4b383", "0000000000000000"),
                              ("unchanged=pass", "unchanged=fail"),
                              ("case=oob state=submitted", "case=unknown state=submitted"),
                              ("retained_leases=3", "retained_leases=0")):
            with self.subTest(before=before), self.assertRaises(ValueError):
                validator.validate([guests[0], guests[1].replace(before, after)], qemu,
                                   access_threads=4, require_negative_checks=True)
        extras = ("QEMU_UB_GM_METADATA_CRC_FAIL op=103 request=103 expected=0x1 actual=0x2",
                  "QEMU_UB_GM_FAILURE_COMPLETION op=103 status=3 code=pto_ub_gm_bad_memref",
                  "QEMU_UB_GM_LOAD request=103 binding=1 addr=0x80000f80 length=256 map=8 generation=9",
                  "SIM_QEMU_UB_GM_BIND_REGISTER op=103 request=103 bindings=3 requester_cna=0xf002")
        for extra in extras:
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                validator.validate(guests, qemu + "\n" + extra,
                                   access_threads=4, require_negative_checks=True)
        lines = guests[1].splitlines()
        position = next(i for i, line in enumerate(lines) if line.startswith("MANAGED_PTO_NEGATIVE_DATA "))
        lines.insert(position - 1, lines.pop(position))
        with self.assertRaises(ValueError):
            validator.validate([guests[0], "\n".join(lines)], qemu,
                               access_threads=4, require_negative_checks=True)

    def test_negative_cli_requires_explicit_gate_and_preserves_raw_log_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory) / name for name in ("a.log", "b.log", "q.log")]
            guests, qemu = negative_fixture()
            for path, contents in zip(paths, guests + [qemu]):
                path.write_bytes(contents.replace("\n", "\r\n").encode())
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]),
                       "--access-threads", "4"]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            result = subprocess.run(command + ["--require-negative-checks"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["expected_rejections"], 6)
            self.assertEqual([row["sha256"] for row in report["sources"]],
                             [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths])

    def test_owner_requires_identity_counts_post_seal_reads_and_original_completion(self):
        for recovery in (False, True):
            for threads in (0, 4, 8):
                guests, qemu = owned_fixture(recovery, threads)
                result = validator.validate(guests, qemu, recovery, threads)
                self.assertTrue(result["owner_verified"])
                self.assertEqual(result["access_threads"], threads)
                self.assertFalse(result["am2_complete"])
            guests, qemu = owned_fixture(recovery)
            for before, after in (("cpu_pins=5", "cpu_pins=1"),
                                  ("admission_guard=pass", "admission_guard=fail"),
                                  ("after_seal_threads=4", "after_seal_threads=3"),
                                  ("checks=40", "checks=0"),
                                  ("holder_pending=0", "holder_pending=1"),
                                  ("holder_transferred=1", "holder_transferred=0"),
                                  ("state=adopted generation=7", "state=adopted generation=8"),
                                  ("operation_id=102" if recovery else "operation_id=0",
                                   "operation_id=999")):
                with self.subTest(recovery=recovery, field=before), self.assertRaises(ValueError):
                    validator.validate([guests[0], guests[1].replace(before, after)],
                                       qemu, recovery, 4)
            lines = guests[1].splitlines()
            guard = next(line for line in lines if "MANAGED_OWNER node=1 state=closing" in line)
            lines.remove(guard)
            lines.insert(2, guard)
            with self.assertRaises(ValueError):
                validator.validate([guests[0], "\n".join(lines)], qemu, recovery, 4)
        guests, qemu = fixture()
        with self.assertRaisesRegex(ValueError, "owner lifecycle sequence"):
            validator.validate(guests, qemu, access_threads=4)

    def test_owner_cli_gate_requires_matching_thread_count(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory) / name for name in ("a.log", "b.log", "q.log")]
            guests, qemu = owned_fixture(True)
            for path, contents in zip(paths, guests + [qemu]):
                path.write_text(contents)
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]),
                       "--require-timeout-recovery", "--access-threads"]
            result = subprocess.run(command + ["4"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(json.loads(result.stdout)["owner_verified"])
            self.assertEqual(subprocess.run(command + ["3"], capture_output=True).returncode, 1)
            self.assertEqual(subprocess.run(command + ["9"], capture_output=True).returncode, 2)

    def test_timeout_requires_original_completion_and_retained_resources(self):
        guests, qemu = fixture()
        with self.assertRaisesRegex(ValueError, "timeout recovery evidence"):
            validator.validate(guests, qemu, True)
        rows = []
        for round_ in (1, 2):
            rows += [f"MANAGED_PTO_WAIT node=1 dispatch={round_} op_id={100+round_} "
                     "state=timeout retained_leases=3 close_guard=pass release_guard=pass",
                     f"MANAGED_PTO_WAIT node=1 dispatch={round_} op_id={100+round_} "
                     f"state=recovered completion_op={100+round_} pending_op=0"]
        for round_ in (1, 2):
            marker = f"MANAGED_PTO node=1 dispatch={round_} completion_op="
            insert = "\n".join(rows[(round_-1)*2:round_*2]) + "\n"
            guests[1] = guests[1].replace(marker, insert + marker)
        self.assertEqual(validator.validate(guests, qemu, True)["timeout_recoveries"], 2)
        late = guests[1].replace(rows[0] + "\n", "") + "\n" + rows[0]
        with self.assertRaises(ValueError):
            validator.validate([guests[0], late], qemu, True)
        late = guests[1]
        for row in rows:
            late = late.replace(row + "\n", "")
        late += "\n" + "\n".join(rows)
        with self.assertRaisesRegex(ValueError, "order mismatch"):
            validator.validate([guests[0], late], qemu, True)
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu)
        for before, after in (("state=timeout", "state=recovered"),
                              ("retained_leases=3", "retained_leases=0"),
                              ("close_guard=pass", "close_guard=fail"),
                              ("release_guard=pass", "release_guard=fail"),
                              ("pending_op=0", "pending_op=101"),
                              ("completion_op=102 pending", "completion_op=101 pending")):
            changed = [guests[0], guests[1].replace(before, after)]
            with self.subTest(field=before), self.assertRaises((ValueError, KeyError)):
                validator.validate(changed, qemu, True)

    def test_complete_parser_fixture(self):
        guests, qemu = fixture()
        result = validator.validate(guests, qemu)
        self.assertEqual(result["status"], "pass")
        self.assertFalse(result["am2_complete"])
        self.assertEqual(result["dispatches"], 2)

    def test_serial_crlf_retains_the_same_validation_rules(self):
        guests, qemu = fixture()
        guests = [text.replace("\n", "\r\n") for text in guests]
        self.assertEqual(validator.validate(guests, qemu)["status"], "pass")
        guests[0] = guests[0].replace("put_object_count=0", "put_object_count=1")
        with self.assertRaisesRegex(ValueError, "control-plane data operation"):
            validator.validate(guests, qemu)

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
            (2, "addr=0x80000f80", "addr=0x90000f80"),
            (2, "addr=0x80001f80 length=256 map=8", "addr=0x80001f80 length=256 map=9"),
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
                path.write_bytes(contents.replace("\n", "\r\n").encode("utf-8"))
            output = pathlib.Path(directory) / "report.json"
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]), "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(len(json.loads(output.read_text())["sources"]), 3)
            self.assertEqual([row["sha256"] for row in
                              json.loads(output.read_text())["sources"]],
                             [hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in paths])
            paths[2].write_text("")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(output.read_text())["status"], "fail")


if __name__ == "__main__":
    unittest.main()
