import importlib.util
import hashlib
import json
import pathlib
import re
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock


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


def readonly_fixture(recovery=False, negative=False, threads=4):
    """Two synthetic object traces; never a substitute for actual guest evidence."""
    guests, qemu = negative_fixture() if negative else owned_fixture(recovery, threads)
    output_home, output_base = "0x700000200000", 0x80200000
    for node, text in enumerate(guests):
        converted, deferred = [], {}
        for line in text.splitlines():
            if line.startswith("MANAGED_PTO "):
                row = validator.records(line, "MANAGED_PTO")[0]
                if "cpu_base" in row:
                    converted.append(line + f" access={1 if node else 3} object=input")
                    output_map = line.replace("key=k ", "key=k-output ").replace(
                        "0x700000000000", output_home).replace("generation=7", "generation=8").replace(
                        "mapping_id=9", "mapping_id=10").replace("mapping_handle=8", "mapping_handle=18")
                    continue
                if "cpu_verify" in row:
                    vector = int(row["vector"])
                    line += " object=" + ("input" if vector < 2 else "output")
                    if node == 1 and vector >= 2:
                        deferred[vector-1] = line
                        continue
                if "op_id" in row:
                    line += " object_maps=" + ("9,9,10" if row["dispatch"] == "1" else "10,9,10")
            if line.startswith("MANAGED_OWNER "):
                row = validator.records(line, "MANAGED_OWNER")[0]
                if row["state"] == "adopted":
                    converted += [line + " object=input", output_map + " access=3 object=output",
                        line.replace("generation=7", "generation=8").replace("mapping_id=9", "mapping_id=10") +
                        " object=output", f"MANAGED_OWNER_PAIR node={node} shared_domain=1 "
                        "input_mapping_id=9 output_mapping_id=10"]
                    if node == 0:
                        converted += [f"MANAGED_PTO node=0 inputs_published=1 version=1 bytes=65536 object={obj}"
                                      for obj in ("input", "output")]
                    continue
                if row["state"] == "closing":
                    converted += [line.replace("compute_pins=3", "compute_pins=1").replace(
                        "in_flight_pins=3", "in_flight_pins=1") + " object=input",
                        line.replace(f"threads={threads}", "threads=0").replace(
                        f"cpu_pins={threads+1}", "cpu_pins=1").replace("compute_pins=3", "compute_pins=2").replace(
                        "in_flight_pins=3", "in_flight_pins=2") + " object=output"]
                    continue
                if row["state"] == "drained":
                    converted.append(line + " object=output")
                line += " object=input"
            if line.startswith("MANAGED_PTO_NEGATIVE_DATA "):
                vector = int(validator.records(line, "MANAGED_PTO_NEGATIVE_DATA")[0]["vector"])
                line += " object=" + ("input" if vector < 2 else "output")
            if line.startswith("MANAGED_PTO_RESULT "):
                line += " objects=2 readonly_inputs=1"
            converted.append(line)
            if node == 1 and "cpu_verify=" in line and "vector=1 " in line:
                converted += ["MANAGED_PTO_READONLY node=1 object=input probe=sdk-write-denied "
                    "cpu_rc=-13 compute_rc=-13 cpu_pins=1 compute_pins=0 in_flight_pins=0",
                    "MANAGED_PTO_READONLY node=1 object=input probe=cpu-write-denied "
                    "address=0x700000000f80 signal=11 code=2 unchanged=pass checksum=0123456789abcdef"]
            if node == 1 and line.startswith("MANAGED_PTO ") and "completion_op=" in line:
                round_ = int(validator.records(line, "MANAGED_PTO")[0]["dispatch"])
                converted.append(deferred[round_])
                if round_ == 2:
                    converted.append("MANAGED_PTO_READONLY node=1 object=input probe=input-unchanged "
                                     "bytes=65536 checksum=0123456789abcdef unchanged=pass")
        guests[node] = "\n".join(converted)
    converted = []
    for line in qemu.splitlines():
        parts = line.split()
        row = dict(part.split("=", 1) for part in parts[1:])
        output_binding = row.get("binding") == "3" or (row.get("request") == "102" and row.get("binding") == "1")
        if output_binding:
            if parts[0] == "QEMU_UB_GM_GSVA_BIND":
                row.update(segment="0x7", home_va=output_home, epoch="8", lease_epoch="9", backing_token="10")
            for key in ("addr", "local"):
                if key in row:
                    row[key] = hex(int(row[key], 0) - 0x80000000 + output_base)
            if "map" in row:
                row.update(map="18", generation="19")
        converted.append(parts[0] + " " + " ".join(f"{key}={value}" for key, value in row.items()))
    return guests, "\n".join(converted)


def reference_fixture():
    """Synthetic V2 parser fixture; does not claim a real guest execution."""
    guests, qemu = readonly_fixture()
    payload = bytearray(4352)
    for vector, start in ((0, 0), (1, 4096)):
        payload[start:start + 256] = struct.pack("<64f", *(
            i % (8 if vector == 0 else 4) + 1 for i in range(64)))
    checksum = int(validator.checksum64(payload), 16)
    reference = struct.pack("<QHHHHIIQQQQQIIQQQ96s64s", 0x514f424d4d524546,
        2, 1, 2, 0, 0, 0, 2, int(validator.checksum64(b"k"), 16), 3968, 4352,
        checksum, 256, 1, 7, 9, 65536, b"k", b"guest-node-0").hex()
    for node, text in enumerate(guests):
        actions = (1, 2, 3, 4) if node == 0 else (4, 5)
        rows = [f"MANAGED_PTO_REFERENCE node={node} action={action} generation=7 version=2 status=ok"
                for action in actions]
        if node == 0:
            rows.insert(1, "MANAGED_PTO node=0 inputs_published=1 version=1 bytes=65536 object=input")
        rows.append(f"MANAGED_PTO_REFERENCE node={node} action=mapped reference={reference}")
        for line in text.splitlines():
            if node == 0 and "inputs_published=" in line and "object=input" in line:
                continue
            if "cpu_base=" in line and "object=input" in line:
                line = line.replace("cpu_base=0x700000000000", "cpu_base=0x700000000f80").replace(
                    "logical_bytes=65536", "logical_bytes=4352").replace("access=3", "access=1")
            if line.startswith("MANAGED_PTO_RESULT "):
                line += " reference_inputs=1"
            line = line.replace("offsets=3968,8064,12160", "offsets=0,4096,12160").replace(
                "offsets=12160,8064,16256", "offsets=12160,4096,16256")
            if "probe=input-unchanged" in line:
                line = line.replace("bytes=65536", "bytes=4352")
            line = line.replace("checksum=0123456789abcdef", f"checksum={checksum:016x}")
            rows.append(line)
        guests[node] = "\n".join(rows)
    return guests, qemu


def queued_fixture(readonly=True, threads=4):
    """Synthetic two-caller evidence; no device execution is represented."""
    guests, qemu = readonly_fixture(threads=threads) if readonly else owned_fixture(threads=threads)
    lines = guests[1].splitlines()
    submits = [line for line in lines if line.startswith("MANAGED_PTO ") and "retained_leases=" in line]
    completes = [line for line in lines if line.startswith("MANAGED_PTO ") and "completion_op=" in line]
    outputs = [line for line in lines if line.startswith("MANAGED_PTO ") and "cpu_verify=" in line
               and validator.records(line, "MANAGED_PTO")[0]["vector"] in ("2", "3")]
    closing = [line for line in lines if line.startswith("MANAGED_OWNER ") and "state=closing" in line]
    unchanged = [line for line in lines if "probe=input-unchanged" in line]
    removed = set(submits + completes + outputs + closing + unchanged)
    batch = []
    for index, line in enumerate(submits):
        if index == 1:
            line = line.replace("12160,8064,16256", "3968,8064,16256").replace(
                "object_maps=10,9,10", "object_maps=9,9,10")
        batch += [line, f"MANAGED_PTO_QUEUE node=1 state=queued dispatch={index+1} op_id={101+index} "
                  f"queued_requests={index+1} retained_leases=3 metadata_iova={0x1000*(index+1):#x}"]
    for line in closing:
        count = (2 if "object=output" in line else 4) if readonly else 6
        batch.append(line.replace("compute_pins=0", f"compute_pins={count}").replace(
            "in_flight_pins=0", f"in_flight_pins={count}").replace("operation_id=0", "operation_id=102"))
    batch.append("MANAGED_PTO_BATCH node=1 state=overlap callers=2 queued_requests=2 retained_leases=6 "
                 "metadata_distinct=1 close_guard=pass release_guard=pass op_ids=101,102")
    for count, index in enumerate((1, 0)):
        batch += [f"MANAGED_PTO_QUEUE node=1 state=completed dispatch={index+1} op_id={101+index} "
                  f"queued_requests={1-count}", completes[index]]
    batch.append("MANAGED_PTO_BATCH node=1 state=drained joined_callers=2 queued_requests=0 compute_leases=0")
    batch += outputs + unchanged
    converted = []
    for line in lines:
        if line == submits[0]:
            converted += batch
        if line not in removed:
            converted.append(line)
    guests[1] = "\n".join(converted)
    for node, text in enumerate(guests):
        result = []
        for line in text.splitlines():
            if line.startswith("MANAGED_PTO ") and "cpu_verify=" in line and "vector=3 " in line:
                line = re.sub(r"path=\S+", "path=" + ("remote-" if node == 0 else "") +
                              "tstore-cpu-independent", line)
                line = re.sub(r"checksum=\S+", "checksum=" + validator.oracle_checksum(2), line)
            if line.startswith("MANAGED_PTO_RESULT "):
                line += " concurrent_requests=2"
            result.append(line)
        guests[node] = "\n".join(result)
    rows = [(line.split()[0], validator.records(line, line.split()[0])[0]) for line in qemu.splitlines()]
    first_input = {prefix: row for prefix, row in rows if row.get("request") == "101" and
                   row.get("binding") == "1"}
    headers, bodies = [], {"101": [], "102": []}
    for prefix, row in rows:
        if row.get("request") == "102" and row.get("binding") == "1":
            row = {key: ("102" if key in ("request", "op") else value)
                   for key, value in first_input[prefix].items()}
        line = prefix + " " + " ".join(f"{key}={value}" for key, value in row.items())
        if prefix in ("QEMU_UB_GM_LOAD", "QEMU_UB_GM_STORE", "QEMU_UB_GM_FENCE", "QEMU_UB_GM_UNBIND"):
            bodies[row["request"]].append(line)
        else:
            headers.append(line)
    return guests, "\n".join(headers + bodies["102"] + bodies["101"])


def large_fixture(layout="nd", recovery=False, negative=False, readonly=True, concurrent_requests=1,
                  strict_io_version=1):
    """Synthetic profile/chunk parser fixture; never actual transport evidence."""
    guests, qemu = (queued_fixture(readonly) if concurrent_requests == 2 else
                    readonly_fixture(recovery, negative) if readonly else
                    negative_fixture() if negative else owned_fixture(recovery))
    elements, object_bytes, offsets = validator.GEOMETRIES[layout]
    tensor_bytes = elements * 4
    chunk_limit = {1: 4055, 2: 3967}[strict_io_version]
    old_offsets = validator.GEOMETRIES["cross-page"][2]
    for node, text in enumerate(guests):
        lines = []
        for line in text.splitlines():
            if not line.startswith("MANAGED_"):
                lines.append(line)
                continue
            prefix = line.split()[0]
            row = validator.records(line, prefix)[0]
            if "logical_bytes" in row:
                row["logical_bytes"] = str(object_bytes)
            if "vector" in row:
                vector = int(row["vector"])
                row["bytes"] = str(tensor_bytes)
                row["checksum"] = (validator.checksum64(validator.struct.pack(
                    f"<{elements}I", *([0x7fc00001] * elements)))
                    if prefix == "MANAGED_PTO_NEGATIVE_DATA" and vector >= 2 else
                    validator.oracle_checksum(2 if vector == 3 and concurrent_requests == 2 else vector, elements))
            if "inputs_published" in row or row.get("probe") == "input-unchanged":
                row["bytes"] = str(object_bytes)
            if row.get("probe") == "cpu-write-denied":
                row["address"] = hex(0x700000000000 + offsets[0])
            if "offsets" in row:
                views = (0, 1, 2) if row["dispatch"] == "1" else (0 if concurrent_requests == 2 else 2, 1, 3)
                row.update(offsets=",".join(str(offsets[v]) for v in views),
                           tensor_bytes=str(tensor_bytes))
            lines.append(prefix + " " + " ".join(f"{k}={v}" for k, v in row.items()))
        guests[node] = "\n".join(lines)
    lines, bindings, authorizations, chunk_id = [], {}, {}, 1000
    for line in qemu.splitlines():
        prefix = line.split()[0]
        row = validator.records(line, prefix)[0]
        views = (0 if concurrent_requests == 2 else 2, 1, 3) if row.get("request") == "102" else (0, 1, 2)
        if "binding" in row:
            vector = views[int(row["binding"]) - 1]
            for key in ("local", "addr"):
                if key in row:
                    row[key] = hex(int(row[key], 0) - old_offsets[vector] + offsets[vector])
            if "length" in row:
                row["length"] = str(tensor_bytes)
            key = row["request"], row["binding"]
            if prefix.endswith("_AUTHORIZE"):
                authorizations[key] = row
            if prefix == "QEMU_UB_GM_GSVA_BIND":
                bindings[key] = row
            if prefix in ("QEMU_UB_GM_LOAD", "QEMU_UB_GM_STORE"):
                for done in range(0, tensor_bytes, chunk_limit):
                    chunk_id += 1
                    size = min(chunk_limit, tensor_bytes - done)
                    backing = bindings[key]
                    peer = authorizations[key]["peer_cna"]
                    address = int(backing["home_va"], 0) + offsets[vector] + done
                    lines += [f"ubc sim_dec read send req={chunk_id} uba={address:#x} "
                        f"len={size} done={done} dcna={peer} token={backing['backing_token']} local_cna=0x2",
                        f"ubc sim_dec rx read_resp req={chunk_id} status=0 data_len={size} "
                        f"wire_len={size} copy_len={size} expect={size} peer={peer}"]
        if prefix == "QEMU_UB_GM_UNBIND":
            row["load_bytes"] = str(2 * tensor_bytes)
            if row["store_bytes"] != "0":
                row["store_bytes"] = str(tensor_bytes)
        lines.append(prefix + " " + " ".join(f"{k}={v}" for k, v in row.items()))
    return guests, "\n".join(lines)


def home_fixture(home, layout="cross-page", recovery=False, negative=False,
                 concurrent_requests=1, readonly=True):
    """Synthetic physical-node/role fixture; never changes a runtime log."""
    if concurrent_requests == 2:
        guests, qemu = (queued_fixture(readonly) if layout == "cross-page" else
                       large_fixture(layout, readonly=readonly, concurrent_requests=2))
    else:
        guests, qemu = (readonly_fixture(recovery=recovery, negative=negative) if layout == "cross-page"
                       else large_fixture(layout=layout, readonly=True, recovery=recovery, negative=negative))
    if home == 1:
        qemu = qemu.replace("requester_cna=0xf002", "requester_cna=0xf001")
        qemu = qemu.replace("peer_cna=0x1", "peer_cna=0x2").replace("dcna=0x1", "dcna=0x2")
        qemu = qemu.replace("peer=0x1", "peer=0x2").replace("local_cna=0x2", "local_cna=0x1")
    physical = [None, None]
    for role, text in enumerate(guests):
        node = home if role == 0 else 1 - home
        lines = []
        for line in text.splitlines():
            line = re.sub(r"\bnode=[01]\b", f"node={node}", line)
            if "cpu_base=" in line:
                line += f" home_node={home} home_provider=guest-node-{home} requester_cna={0xf001+node:#x}"
                if role == 0:
                    mapping = validator.records(line, "MANAGED_PTO")[0]
                    bound = next(row for row in validator.records(qemu, "QEMU_UB_GM_GSVA_BIND")
                                 if row["home_va"] == mapping["cpu_base"])
                    lines.append(f"obmm-worker generation={mapping['generation']} phase=exported "
                                 f"segment={int(bound['segment'], 0)} export=2")
            lines.append(line)
        physical[node] = "\n".join(lines)
    return physical, qemu


def model_probe_fixture():
    """Synthetic parser input; never used as actual guest evidence."""
    hidden = b"".join(struct.pack("<e", ((i % 31) - 15) / 32) for i in range(1024))
    reference = struct.pack("<QHHHHIIQQQQQIIQQQ96s64s", 0x514f424d4d524546,
        2, 5, 2, 0, 1, 1, 2, int(validator.checksum64(b"k"), 16), 0, 2048,
        int(validator.checksum64(hidden), 16), 256, 1, 7, 9, 262144, b"k", b"guest-node-1").hex()
    guests = []
    for node in (0, 1):
        lines = [f"MANAGED_PTO_CONFIG node={node} model_probe=1 callable=4 home_node=1"]
        for index, role in enumerate(("input", "output")):
            address = 0x700000000000 + index * 0x400000
            key = "k" + ("-output" if index else "")
            lines.append(f"MANAGED_PTO node={node} key={key} generation={7+index} "
                f"cpu_base={address:#x} address={address:#x} logical_bytes={262144 if index else 2048} "
                f"mapping_id={9+index} access={3 if index else 1} object={role} "
                "home_node=1 home_provider=guest-node-1 requester_cna=0xf001")
            if node == 1:
                lines.append(f"obmm-worker phase=exported generation={7+index} segment={6+index} export={8+index}")
        for action in ((4, 5) if node == 0 else (1, 2, 3, 4)):
            lines.append(f"MANAGED_PTO_REFERENCE node={node} action={action} generation=7 version=2 status=ok")
        lines.append(f"MANAGED_PTO_REFERENCE node={node} action=mapped reference={reference}")
        if node == 0:
            lines += ["MANAGED_MODEL node=0 action=submitted op_id=101 callable=4 compute_leases=3 inputs=1",
                      "MANAGED_MODEL node=0 action=completed op_id=101 status=success original_completion=1"]
        lines.append(f"MANAGED_MODEL node={node} action=cpu-verify bytes=64 checksum=0000000000001234 bounds=pass status=pass")
        for role in ("output", "input"):
            lines.append(f"MANAGED_OWNER node={node} state=drained object={role} cpu_pins=0 "
                         "compute_pins=0 in_flight_pins=0 mapping_pending=0 holder_pending=0")
        lines.append(f"MANAGED_PTO_RESULT node={node} status=pass key=k generation=7 objects=2 "
            "mapping_closed=1 holder_released=1 compute_leases=0 readonly_inputs=1 reference_inputs=1")
        lines += ["put_object_count=0", "materialize_object_count=0"]
        if node == 1:
            lines += [f"{metric}=0" for metric in
                      ("live_objects", "backing_allocated_bytes", "address_reserved_bytes",
                       "export_mappings", "import_mappings", "live_refs", "in_flight",
                       "quarantined_objects", "quarantined_bytes")]
        for phase in ("baseline", "drained"):
            lines += [f"mem_service kernel-regions: phase={phase}",
                      "obmm-kernel-regions: version=1 scope=sysfs consistency=non_atomic "
                      "export_handles=0 export_bytes=0 import_handles=0 import_bytes=0"]
        guests.append("\n".join(lines))
    qemu, callbacks = [], []
    for binding, length, address, home, segment in (
            (1, 8768, 0x90000000, 0x700000400000, 7),
            (2, 2048, 0x80000000, 0x700000000000, 6),
            (3, 64, 0x90004000, 0x700000400000, 7)):
        role = "INPUT" if binding < 3 else "OUTPUT"
        qemu.append(f"QEMU_UB_GM_{role}_AUTHORIZE op=101 request=101 binding={binding} "
                    f"length={length} local={address:#x} requester_cna=0xf001 map=8 generation=9")
        qemu.append(f"QEMU_UB_GM_GSVA_BIND request=101 binding={binding} segment={segment} "
                    f"home_va={home:#x} epoch=7 lease_epoch=8 backing_token=9")
        for kind in (("LOAD",) if binding < 3 else ("STORE", "FENCE")):
            callbacks.append(f"QEMU_UB_GM_{kind} request=101 binding={binding} addr={address:#x} "
                        f"length={length} map=8 generation=9")
    qemu += ["SIM_QEMU_UB_GM_BIND_REGISTER op=101 request=101 bindings=3 requester_cna=0xf001",
             *callbacks[:2],
             "qwen3-range-runtime-timing: task_id=101 node=0 layers=[1,2] terminal_owner=0 "
             "decode_step=0 real_forward=1 output_bytes=64", *callbacks[2:],
             "QEMU_UB_GM_UNBIND op=101 request=101 bindings=3 reason=completion_success "
             "load_bytes=10816 store_bytes=64 fences=1 segment_payload_staging_bytes=0"]
    return guests, "\n".join(qemu)


def model_publication_fixture():
    guests, qemu = model_probe_fixture()
    key = ("w5v1/0000000000000064/0000000000000065/00000000/"
           "0000000000000000")
    guests = [text.replace("model_probe=1", "model_probe=1 model_publish=1")
              .replace("bytes=64", "bytes=54680")
              .replace("key=k-output", f"key={key}") for text in guests]
    guests = [f"MANAGED_MODEL_KEY status=ok node={node} producer=0 step=0 "
              f"key={key} network_operations=0\n" + text
              for node, text in enumerate(guests)]
    qemu = qemu.replace("length=64", "length=54680").replace("bytes=64", "bytes=54680")
    refs = []
    sizes = [(4, 16384, 54680, 0x1234), (5, 60784, 2048, 0x8888), (7, 62832, 8232, 0x9999)]
    output_bytes = sizes[0][2]
    for kind, offset, length, checksum in sizes:
        refs.append(struct.pack("<QHHHHIIQQQQQIIQQQ96s64s", 0x514f424d4d524546,
            2, kind, 2, 0, 0, 0, 2, int(validator.checksum64(key.encode()), 16), offset,
            length, checksum, 256, 1, 8, 9, 262144, key.encode(), b"guest-node-1").hex())
    def transition(node, action, view):
        return (f"MANAGED_PTO_REFERENCE node={node} action={action} generation=8 version=2 "
                f"status=ok object=output view={view}\n")
    def mapped(index):
        _, offset, length, _ = sizes[index]
        return (f"MANAGED_MODEL_REFERENCE node=1 action=mapped view={index} reference={refs[index]} "
                f"cpu_base={0x700000400000+offset:#x} logical_bytes={length} mapping_id={20+index}\n")
    def read(index):
        return (f"MANAGED_MODEL_REFERENCE node=1 action=read view={index} bytes={sizes[index][2]} "
                f"checksum={sizes[index][3]:016x} access=1 status=pass\n")
    for node in (0, 1):
        marker = f"MANAGED_MODEL node={node} action=cpu-verify"
        views = (f"MANAGED_MODEL_VIEWS node={node} hidden_offset=60784 hidden_bytes=2048 "
                 "hidden_checksum=0000000000008888 kv_offset=62832 kv_bytes=8232 "
                 f"kv_checksum=0000000000009999 real_layers=1 serialized_bytes={output_bytes} "
                 "terminal_present=0 terminal_step=0 terminal_token=0 "
                 "payload_copies=0 status=pass\n")
        guests[node] = guests[node].replace(marker, views + marker)
    guests[0] = guests[0].replace("MANAGED_MODEL node=0 action=submitted",
        transition(0, 1, 0) + "MANAGED_MODEL node=0 action=submitted")
    staged = ''.join(transition(0, 2, i) +
        f"MANAGED_MODEL_REFERENCE node=0 action=staged view={i} reference={refs[i]}\n" for i in range(3))
    staged += "MANAGED_MODEL_PUBLICATION node=0 state=writer-unmapped holder_retained=1\n"
    staged += transition(0, 3, 2)
    staged += "MANAGED_MODEL_PUBLICATION node=0 state=sealed generation=8 version=2 views=3 payload_copies=0\n"
    guests[0] = guests[0].replace("MANAGED_OWNER node=0 state=drained object=output",
                                 staged + "MANAGED_OWNER node=0 state=drained object=output")
    guests[1] = guests[1].replace("cpu_base=0x700000400000", "cpu_base=0x700000404000")
    guests[1] = guests[1].replace("logical_bytes=262144 mapping_id=10 access=3",
                                 "logical_bytes=54680 mapping_id=10 access=1")
    guests[1] = guests[1].replace("MANAGED_MODEL_VIEWS node=1",
        transition(1, 4, 0) + transition(1, 5, 0) + mapped(0) + "MANAGED_MODEL_VIEWS node=1")
    drained = ("MANAGED_OWNER node=1 state=drained object=output cpu_pins=0 "
               "compute_pins=0 in_flight_pins=0 mapping_pending=0 holder_pending=0")
    sequence = read(0) + drained + '\n'
    for i in (1, 2):
        sequence += transition(1, 4, i) + transition(1, 5, i) + mapped(i) + read(i) + drained + '\n'
    guests[1] = guests[1].replace(drained + '\n', sequence)
    return guests, qemu


class ManagedPtoValidatorTest(unittest.TestCase):
    def test_model_output_publication_requires_sealed_exact_views_and_readers(self):
        guests, qemu = model_publication_fixture()
        result = validator.validate_model_probe(guests, qemu, 54680, 0x1234, publication=True)
        self.assertTrue(result["output_publication_verified"])
        self.assertFalse(result["pp_decode_verified"])
        for node, old, new in (
                (0, "MANAGED_MODEL_KEY status=ok", "MANAGED_MODEL_KEY status=missing"),
                (0, "producer=0 step=0 key=", "producer=1 step=0 key="),
                (1, "step=0 key=", "step=1 key="),
                (0, "key=w5v1/0000000000000064", "key=W5v1/0000000000000064"),
                (1, "network_operations=0", "network_operations=1"),
                (0, "holder_retained=1", "holder_retained=0"),
                (0, "state=writer-unmapped", "state=skipped"),
                (0, "views=3", "views=2"),
                (0, "action=staged view=2", "action=staged view=1"),
                (0, "model_publish=1", "model_publish=0"),
                (1, "mapping_id=21", "mapping_id=20"),
                (1, "action=read view=2", "action=missing view=2"),
                (1, "access=1 status=pass", "access=3 status=pass"),
                (1, "cpu_base=0x700000404000", "cpu_base=0x700000404001"),
                (1, "checksum=0000000000009999", "checksum=0000000000009998"),
                (1, "kv_offset=62832", "kv_offset=62831")):
            changed = list(guests)
            self.assertIn(old, changed[node])
            changed[node] = changed[node].replace(old, new)
            with self.subTest(field=old), self.assertRaises(ValueError):
                validator.validate_model_probe(changed, qemu, 54680, 0x1234, publication=True)
        changed = list(guests)
        lines = changed[0].splitlines()
        unmap = next(i for i, line in enumerate(lines) if "state=writer-unmapped" in line)
        seal = next(i for i, line in enumerate(lines) if "state=sealed" in line)
        lines[unmap], lines[seal] = lines[seal], lines[unmap]
        changed[0] = '\n'.join(lines)
        with self.assertRaises(ValueError):
            validator.validate_model_probe(changed, qemu, 54680, 0x1234, publication=True)

    def test_model_probe_requires_reference_mapping_callbacks_and_actual_layer(self):
        guests, qemu = model_probe_fixture()
        result = validator.validate_model_probe(guests, qemu, 64, 0x1234)
        self.assertEqual(result["status"], "pass")
        self.assertFalse(result["pp_decode_verified"])
        self.assertFalse(result["am2_complete"])
        for metric in ("live_objects", "backing_allocated_bytes", "address_reserved_bytes",
                       "export_mappings", "import_mappings", "live_refs", "in_flight",
                       "quarantined_objects", "quarantined_bytes"):
            for replacement in (f"{metric}=1", "", f"{metric}=0\n{metric}=1"):
                with self.subTest(resource=metric, replacement=replacement), self.assertRaises(ValueError):
                    validator.validate_model_probe(
                        [guests[0], guests[1].replace(f"{metric}=0", replacement)], qemu, 64, 0x1234)
        for old, new in (("length=2048", "length=2049"), ("load_bytes=10816", "load_bytes=10815"),
                         ("store_bytes=64", "store_bytes=63"), ("fences=1", "fences=0"),
                         ("requester_cna=0xf001", "requester_cna=0"),
                         ("real_forward=1", "real_forward=0"), ("task_id=101", "task_id=102"),
                         ("completion_success", "timeout"), ("local=0x90004000", "local=0x90004001"),
                         ("segment_payload_staging_bytes=0", "segment_payload_staging_bytes=64")):
            with self.subTest(qemu_field=old), self.assertRaises(ValueError):
                validator.validate_model_probe(guests, qemu.replace(old, new), 64, 0x1234)
        for old, new in (("original_completion=1", "original_completion=0"),
                         ("checksum=0000000000001234", "checksum=0000000000001235"),
                         ("holder_pending=0", "holder_pending=1"),
                         ("compute_leases=3", "compute_leases=2"),
                         ("access=1", "access=3"), ("action=5", "action=4"),
                         ("put_object_count=0", "put_object_count=1")):
            with self.subTest(guest_field=old), self.assertRaises(ValueError):
                validator.validate_model_probe([guests[0].replace(old, new), guests[1]], qemu, 64, 0x1234)

    def test_reference_mode_preserves_compute_and_ownership_gates(self):
        guests, qemu = reference_fixture()
        result = validator.validate(guests, qemu, access_threads=4,
            require_readonly_inputs=True, require_reference_inputs=True)
        self.assertTrue(result["reference_inputs_verified"])
        self.assertEqual((result["reference"]["offset"], result["reference"]["bytes"]), (3968, 4352))
        self.assertFalse(result["am2_complete"])
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu.replace("home_va=0x700000000000", "home_va=0x700000000f80"),
                access_threads=4, require_readonly_inputs=True, require_reference_inputs=True)
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu, access_threads=4, require_readonly_inputs=True)
        with self.assertRaises(ValueError):
            validator.validate(*readonly_fixture(), access_threads=4,
                require_readonly_inputs=True, require_reference_inputs=True)

    def test_reference_mode_rejects_identity_range_sequence_and_compute_corruption(self):
        guests, qemu = reference_fixture()
        mutations = (
            ("action=3 ", "action=2 "),
            ("generation=7 version=2", "generation=7 version=3"),
            ("logical_bytes=4352", "logical_bytes=65536"),
            ("cpu_base=0x700000000f80", "cpu_base=0x700000000000"),
            ("offsets=0,4096,12160", "offsets=3968,8064,12160"),
            ("completion_op=102", "completion_op=103"),
            ("reference_inputs=1", "reference_inputs=0"),
        )
        for old, new in mutations:
            changed = [text.replace(old, new) for text in guests]
            with self.subTest(field=old), self.assertRaises(ValueError):
                validator.validate(changed, qemu, access_threads=4,
                    require_readonly_inputs=True, require_reference_inputs=True)
        encoded = validator.records(guests[1], "MANAGED_PTO_REFERENCE")[-1]["reference"]
        for byte in (0, 8, 12, 16, 24, 40, 48, 56, 68, 72, 80, 88, 96, 192):
            altered = bytearray.fromhex(encoded)
            altered[byte] ^= 1
            with self.subTest(byte=byte), self.assertRaises((ValueError, UnicodeError)):
                validator.validate([guests[0], guests[1].replace(encoded, altered.hex())], qemu,
                    access_threads=4, require_readonly_inputs=True, require_reference_inputs=True)

    def test_queued_requests_verify_independent_outputs_both_homes_and_full_geometry(self):
        for home in (0, 1):
            for layout in ("cross-page", "nd", "tail"):
                for readonly in (False, True):
                    with self.subTest(home=home, layout=layout, readonly=readonly):
                        guests, qemu = home_fixture(home, layout, concurrent_requests=2, readonly=readonly)
                        result = validator.validate(guests, qemu, False, 4, False, readonly,
                                                    layout, home, 2)
                        self.assertEqual(result["concurrent_requests"], 2)
                        self.assertTrue(result["queued_overlap_verified"])
                        self.assertFalse(result["device_parallel_execution_verified"])
                        self.assertFalse(result["am2_complete"])
                        if layout != "cross-page":
                            self.assertTrue(result["transport_chunks"]["request_response_verified"])
                        with self.assertRaises(ValueError):
                            validator.validate(guests, qemu, False, 4, False, readonly, layout, home, 1)

    def test_queued_gate_rejects_lost_replayed_unpinned_or_early_released_requests(self):
        guests, qemu = home_fixture(1, concurrent_requests=2)
        mutations = (
            ("metadata_iova=0x2000", "metadata_iova=0x1000"),
            ("metadata_iova=0x2000", "metadata_iova=0x2001"),
            ("retained_leases=6", "retained_leases=5"),
            ("op_ids=101,102", "op_ids=101,101"),
            ("state=overlap callers=2", "state=overlap callers=1"),
            ("state=completed dispatch=1 op_id=101", "state=completed dispatch=1 op_id=102"),
            ("state=completed dispatch=2 op_id=102 queued_requests=1",
             "state=completed dispatch=2 op_id=102 queued_requests=0"),
            ("joined_callers=2", "joined_callers=1"),
            ("compute_pins=4", "compute_pins=3"),
            ("in_flight_pins=4", "in_flight_pins=3"),
            ("operation_id=102", "operation_id=101"),
            ("metadata_distinct=1", "metadata_distinct=0"),
            ("release_guard=pass", "release_guard=fail"),
            ("concurrent_requests=2", "concurrent_requests=1"),
        )
        for old, new in mutations:
            changed = list(guests)
            self.assertIn(old, changed[0])
            changed[0] = changed[0].replace(old, new, 1)
            with self.subTest(mutation=old), self.assertRaises(ValueError):
                validator.validate(changed, qemu, False, 4, False, True, "cross-page", 1, 2)
        for state in ("queued", "completed", "overlap", "drained"):
            changed = list(guests)
            lines = changed[0].splitlines()
            index = next(i for i, line in enumerate(lines) if line.startswith(
                ("MANAGED_PTO_QUEUE ", "MANAGED_PTO_BATCH ")) and f"state={state}" in line.split())
            lines.pop(index)
            changed[0] = "\n".join(lines)
            with self.subTest(missing=state), self.assertRaises(ValueError):
                validator.validate(changed, qemu, False, 4, False, True, "cross-page", 1, 2)

    def test_queued_oracle_and_callbacks_reject_dependent_input_and_overlapping_outputs(self):
        guests, qemu = queued_fixture()
        for changed in (qemu.replace("0x80203f80", "0x80202f80"), readonly_fixture()[1]):
            self.assertNotEqual(changed, qemu)
            with self.assertRaises(ValueError):
                validator.validate(guests, changed, False, 4, False, True, concurrent_requests=2)
        changed = [text.replace("path=" + ("remote-" if i == 0 else "") +
                    "tstore-cpu-independent vector=3 version=3 checksum=" + validator.oracle_checksum(2),
                    "path=" + ("remote-" if i == 0 else "") +
                    "tstore-cpu-independent vector=3 version=3 checksum=" + validator.oracle_checksum(3))
                   for i, text in enumerate(guests)]
        self.assertNotEqual(changed, guests)
        with self.assertRaises(ValueError):
            validator.validate(changed, qemu, False, 4, False, True, concurrent_requests=2)

    def test_queued_cli_requires_explicit_mode_and_keeps_original_log_hashes(self):
        guests, qemu = home_fixture(1, concurrent_requests=2)
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory)/name for name in ("a.log", "b.log", "qemu.log")]
            for path, contents in zip(paths, [*guests, qemu]):
                path.write_bytes(contents.encode())
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]), "--node-b", str(paths[1]),
                       "--qemu-a", str(paths[2]), "--home-node", "1", "--require-readonly-inputs",
                       "--access-threads", "4"]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            result = subprocess.run(command + ["--concurrent-requests", "2"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual([row["sha256"] for row in report["sources"]],
                             [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths])

    def test_home_export_contract_ignores_worker_prose_and_rejects_malformed_exports(self):
        for home in (0, 1):
            guests, qemu = home_fixture(home)
            notices = (
                f"obmm-worker starting provider refresh node=guest-node-{home}\n"
                "obmm-worker initial refresh provider_directory_ready=1\n"
                "obmm-worker work key=fixture generation=1 state=allocating\n"
                "obmm-worker generation=0 phase=worker-start segment=0 export=0\n"
            )
            guests[home] = notices + guests[home]
            self.assertTrue(validator.validate(guests, qemu, False, 4, False, True,
                                              home_node=home)["home_export_verified"])
            for invalid in ("unexpected ", "phase=reserved ", "generation=999 "):
                changed = list(guests)
                changed[home] = changed[home].replace("phase=exported ", "phase=exported " + invalid, 1)
                with self.subTest(home=home, invalid=invalid), self.assertRaises(ValueError):
                    validator.validate(changed, qemu, False, 4, False, True, home_node=home)

    def test_home_cli_selects_actual_compute_log_and_hashes_original_bytes(self):
        guests, qemu = home_fixture(1)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = [root/name for name in ("nodeA.log", "nodeB.log", "qemuA.log", "qemuB.log")]
            for path, text in zip(paths, [*guests, qemu, "unrelated home QEMU diagnostic"]):
                path.write_bytes(text.encode())
            output = root/"report.json"
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]), "--node-b", str(paths[1]),
                "--qemu-b", str(paths[3]), "--home-node", "1", "--require-readonly-inputs",
                "--access-threads", "4", "--output", str(output)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)
            result = subprocess.run(command+["--qemu-a", str(paths[2])], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            report = json.loads(output.read_text())
            self.assertEqual([row["path"] for row in report["sources"]], [str(path) for path in paths[:3]])
            self.assertEqual(report["sources"][2]["sha256"], hashlib.sha256(qemu.encode()).hexdigest())

    def test_home_roles_preserve_geometry_negatives_and_original_completion(self):
        for home in (0, 1):
            for layout in ("cross-page", "nd", "tail"):
                for recovery, negative in ((False, False), (True, False), (False, True)):
                    with self.subTest(home=home, layout=layout, recovery=recovery, negative=negative):
                        guests, qemu = home_fixture(home, layout, recovery, negative)
                        result = validator.validate(guests, qemu, recovery, 4, negative, True, layout, home)
                        self.assertEqual(result["home_node"], home)
                        self.assertEqual(result["compute_node"], 1-home)
                        self.assertTrue(result["home_export_verified"])

    def test_home_gate_rejects_wrong_node_provider_export_and_compute_requester(self):
        guests, qemu = home_fixture(1)
        for changed in (
            list(reversed(guests)),
            [text.replace("home_provider=guest-node-1", "home_provider=guest-node-0") for text in guests],
            [text.replace("home_node=1", "home_node=0") for text in guests],
            [text.replace("phase=exported", "phase=reserved") for text in guests],
            [text.replace("export=2", "export=0") for text in guests],
            [guests[0].replace("MANAGED_PTO node=0 dispatch=1", "MANAGED_PTO node=1 dispatch=1"), guests[1]],
        ):
            with self.assertRaises((ValueError, KeyError)):
                validator.validate(changed, qemu, False, 4, False, True, home_node=1)
        for changed in (qemu.replace("requester_cna=0xf001", "requester_cna=0xf002"),
                        qemu.replace("segment=0x6", "segment=0x16")):
            with self.assertRaises(ValueError):
                validator.validate(guests, changed, False, 4, False, True, home_node=1)

    def test_bulk_parser_does_not_reparse_scalar_io_as_pto_metadata(self):
        _, qemu = large_fixture(negative=True)
        scalar = (
            "ubc sim_dec read send req=10 uba=0xffffffe00000 len=4 done=0 "
            "dcna=0x1 token=96 local_cna=0x2\n"
            "ubc sim_dec rx read_resp req=10 status=0 data_len=4 wire_len=4 "
            "copy_len=4 expect=4 peer=0x1\n"
        )
        with mock.patch.object(validator, "records", wraps=validator.records) as parse:
            result = validator.validate_transport_chunks(
                scalar + "unrelated guest diagnostic\n" + qemu, ["101", "102"],
                validator.GEOMETRIES["nd"])
        self.assertEqual(result["load"], 136)
        self.assertEqual(result["store"], 34)
        for call in parse.call_args_list:
            text, prefix = call.args
            if "\n" not in text and text.startswith("ubc sim_dec "):
                self.assertFalse(prefix.startswith(("QEMU_UB_GM_", "SIM_QEMU_UB_GM_")))

    def test_large_chunks_distinguish_legacy_bootstrap_from_registered_pto(self):
        guests, qemu = large_fixture(readonly=True)
        # Synthetic legacy bootstrap page read; this is not strict PTO I/O.
        bootstrap = "\n".join((
            "ubc sim_dec read send req=206 uba=0xffffffe00000 len=4079 done=0 "
            "dcna=0x1 token=96 local_cna=0x2",
            "ubc sim_dec rx read_resp req=206 status=0 data_len=4079 wire_len=4079 "
            "copy_len=4079 expect=4079 peer=0x1",
            "ubc sim_dec read send req=207 uba=0xffffffe00fef len=17 done=4079 "
            "dcna=0x1 token=96 local_cna=0x2",
            "ubc sim_dec rx read_resp req=207 status=0 data_len=17 wire_len=17 "
            "copy_len=17 expect=17 peer=0x1")) + "\n"
        result = validator.validate(guests, bootstrap + qemu, False, 4, False, True, "nd")
        self.assertEqual(result["transport_chunks"]["non_pto_bulk_sends"], 2)
        self.assertEqual(result["transport_chunks"]["scope"], "registered-pto-dispatches")
        self.assertEqual(result["transport_chunks"]["load"], 68)
        marker = "ubc sim_dec read send req=1001"
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu.replace(marker, bootstrap + marker, 1),
                               False, 4, False, True, "nd")
        with self.assertRaisesRegex(ValueError, "outside a registered PTO dispatch"):
            validator.validate(guests, bootstrap.replace("token=96", "token=9") + qemu,
                               False, 4, False, True, "nd")

    def test_large_chunks_require_complete_nonoverlapping_dispatch_windows(self):
        _, qemu = large_fixture()
        register = next(line for line in qemu.splitlines()
                        if line.startswith("SIM_QEMU_UB_GM_BIND_REGISTER"))
        unbind = next(line for line in qemu.splitlines() if line.startswith("QEMU_UB_GM_UNBIND"))
        mutations = (qemu.replace(register, "", 1),
                     qemu.replace(register, register + "\n" + register, 1),
                     qemu.replace(unbind, "", 1),
                     qemu.replace(unbind, unbind.replace("request=101", "request=999"), 1))
        for changed in mutations:
            with self.assertRaises(ValueError):
                validator.validate_transport_chunks(changed, ["101", "102"],
                                                    validator.GEOMETRIES["nd"])

    def test_large_geometry_requires_complete_remote_chunks_and_all_existing_gates(self):
        for layout in ("nd", "tail"):
            for recovery, negative, readonly in ((False, False, False), (False, False, True),
                                                 (True, False, True), (False, True, True)):
                guests, qemu = large_fixture(layout, recovery, negative, readonly)
                result = validator.validate(guests, qemu, recovery, 4, negative, readonly, layout)
                self.assertEqual(result["layout"], layout)
                chunks = result["transport_chunks"]
                self.assertEqual(chunks["load"], (8 if negative else 4) * 17)
                self.assertEqual(chunks["store"], 34)
                self.assertEqual(chunks["store_bytes"], 2 * result["tensor_bytes"])
                self.assertTrue(chunks["request_response_verified"])
                self.assertFalse(result["am2_complete"])
                with self.assertRaises(ValueError):
                    validator.validate(guests, qemu, recovery, 4, negative, readonly)

    def test_large_geometry_rejects_missing_replayed_misrouted_and_short_chunks(self):
        guests, qemu = large_fixture(negative=True)
        mutations = (("len=4055 done=4055", "len=4055 done=4056"),
                     ("len=656 done=64880", "len=655 done=64880"),
                     ("uba=0x700000000000", "uba=0x700000000004"),
                     ("dcna=0x1 token=9", "dcna=0x2 token=9"),
                     ("dcna=0x1 token=9", "dcna=0x1 token=10"),
                     ("status=0 data_len=4055", "status=1 data_len=4055"),
                     ("wire_len=4055 copy_len=4055", "wire_len=4054 copy_len=4055"),
                     ("copy_len=4055 expect=4055", "copy_len=4054 expect=4055"),
                     ("expect=4055 peer=0x1", "expect=4055 peer=0x3"),
                     ("read_resp req=1001", "read_resp req=9999"),
                     ("read send req=1002", "read send req=1001"))
        for before, after in mutations:
            self.assertIn(before, qemu)
            with self.subTest(before=before), self.assertRaises(ValueError):
                validator.validate(guests, qemu.replace(before, after, 1), False, 4, True, True, "nd")
        lines = qemu.splitlines()
        for needle in ("read send req=1001", "read_resp req=1001"):
            line = next(row for row in lines if needle in row)
            for changed in (qemu.replace(line, "", 1), qemu + "\n" + line):
                with self.subTest(needle=needle), self.assertRaises(ValueError):
                    validator.validate(guests, changed, False, 4, True, True, "nd")
        no_chunks = "\n".join(line for line in lines if not line.startswith("ubc sim_dec "))
        with self.assertRaisesRegex(ValueError, "transport evidence"):
            validator.validate(guests, no_chunks, False, 4, True, True, "nd")

    def test_v2_chunks_require_explicit_version_and_preserve_exact_ranges(self):
        runner = (ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh").read_text()
        self.assertIn("--strict-io-version 2", runner)
        for layout in ("nd", "tail"):
            guests, qemu = large_fixture(layout, strict_io_version=2)
            result = validator.validate(guests, qemu, access_threads=4,
                                        require_readonly_inputs=True, layout=layout,
                                        strict_io_version=2)
            self.assertEqual(result["transport_chunks"]["chunk_limit_bytes"], 3967)
            self.assertEqual(result["transport_chunks"]["strict_io_version"], 2)
            self.assertEqual(result["transport_chunks"]["load"], 68)
            self.assertEqual(result["transport_chunks"]["store"], 34)
            with self.assertRaises(ValueError):
                validator.validate(guests, qemu, access_threads=4,
                                   require_readonly_inputs=True, layout=layout)
            mutations = (("len=3967 done=3967", "len=3967 done=3968"),
                         ("len=3967 done=0", "len=4055 done=0"),
                         ("wire_len=3967 copy_len=3967", "wire_len=3966 copy_len=3967"),
                         ("read send req=1002", "read send req=1001"))
            for before, after in mutations:
                self.assertIn(before, qemu)
                with self.subTest(layout=layout, mutation=before), self.assertRaises(ValueError):
                    validator.validate(guests, qemu.replace(before, after, 1), access_threads=4,
                                       require_readonly_inputs=True, layout=layout,
                                       strict_io_version=2)
        guests, qemu = large_fixture()
        with self.assertRaises(ValueError):
            validator.validate(guests, qemu, access_threads=4, require_readonly_inputs=True,
                               layout="nd", strict_io_version=2)

    def test_v2_chunk_cli_requires_selected_wire_profile(self):
        guests, qemu = large_fixture("tail", strict_io_version=2)
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory) / name for name in ("a.log", "b.log", "q.log")]
            for path, text in zip(paths, guests + [qemu]):
                path.write_text(text)
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]),
                       "--require-readonly-inputs", "--access-threads", "4", "--layout", "tail"]
            for version, expected in (("1", 1), ("2", 0), ("3", 2)):
                checked = subprocess.run(command + ["--strict-io-version", version],
                                         capture_output=True, text=True, timeout=5)
                self.assertEqual(checked.returncode, expected, checked.stdout + checked.stderr)

    def test_large_cli_requires_profile_and_preserves_original_source_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory) / name for name in ("a.log", "b.log", "q.log")]
            guests, qemu = large_fixture("tail", recovery=True)
            for path, data in zip(paths, guests + [qemu]):
                path.write_bytes(data.replace("\n", "\r\n").encode())
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]),
                       "--require-readonly-inputs", "--require-timeout-recovery", "--access-threads", "4"]
            self.assertEqual(subprocess.run(command, capture_output=True, timeout=5).returncode, 1)
            result = subprocess.run(command + ["--layout", "tail"], capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["tensor_bytes"], 65024)
            self.assertEqual(report["transport_chunks"]["load"], 68)
            for row, path in zip(report["sources"], paths):
                self.assertEqual(row["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_readonly_objects_preserve_all_compute_negative_and_recovery_gates(self):
        for recovery, negative, threads in ((False, False, 0), (False, False, 4),
                                            (True, False, 4), (False, True, 4)):
            guests, qemu = readonly_fixture(recovery, negative, threads)
            with self.subTest(recovery=recovery, negative=negative, threads=threads):
                result = validator.validate(guests, qemu, recovery, threads, negative, True)
                self.assertEqual(result["objects"], 2)
                self.assertTrue(result["readonly_inputs_verified"])
                self.assertTrue(result["owner_verified"])
                self.assertFalse(result["am2_complete"])
                with self.assertRaises(ValueError):
                    validator.validate(guests, qemu, recovery, threads, negative)

    def test_readonly_gate_rejects_unproved_permissions_backing_and_owner_lifetime(self):
        guests, qemu = readonly_fixture(negative=True)
        for before, after in (("access=1 object=input", "access=3 object=input"),
                              ("cpu_rc=-13", "cpu_rc=0"),
                              ("compute_rc=-13", "compute_rc=0"),
                              ("signal=11 code=2", "signal=11 code=0"),
                              ("address=0x700000000f80", "address=0x700000200f80"),
                              ("bytes=65536 checksum=0123456789abcdef", "bytes=65536 checksum=0000000000000000"),
                              ("object_maps=9,9,10", "object_maps=9,9,9"),
                              ("shared_domain=1", "shared_domain=0"),
                              ("cpu_pins=1 holder_transferred=1 object=output",
                               "cpu_pins=0 holder_transferred=1 object=output"),
                              ("objects=2", "objects=1")):
            self.assertIn(before, guests[1])
            with self.subTest(before=before), self.assertRaises(ValueError):
                validator.validate([guests[0], guests[1].replace(before, after)], qemu,
                                   access_threads=4, require_negative_checks=True, require_readonly_inputs=True)
        for before, after in (("GSVA_BIND request=107 binding=3 segment=0x7", "GSVA_BIND request=107 binding=3 segment=0x6"),
                              ("LOAD request=102 binding=1 addr=0x80202f80", "LOAD request=102 binding=1 addr=0x80002f80"),
                              ("STORE request=101 binding=3 addr=0x80202f80", "STORE request=101 binding=3 addr=0x80002f80"),
                              ("home_va=0x700000200000", "home_va=0x700000000000")):
            self.assertIn(before, qemu)
            with self.subTest(before=before), self.assertRaises(ValueError):
                validator.validate(guests, qemu.replace(before, after), access_threads=4,
                                   require_negative_checks=True, require_readonly_inputs=True)
        lines = guests[1].splitlines()
        for prefix in ("MANAGED_PTO_READONLY ", "MANAGED_OWNER_PAIR "):
            line = next(row for row in lines if row.startswith(prefix))
            for changed in (guests[1].replace(line, ""), guests[1] + "\n" + line):
                with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                    validator.validate([guests[0], changed], qemu, access_threads=4,
                                       require_negative_checks=True, require_readonly_inputs=True)
        line = next(row for row in lines if "probe=input-unchanged" in row)
        with self.assertRaises(ValueError):
            validator.validate([guests[0], guests[1].replace(line, "") + "\n" + line], qemu,
                               access_threads=4, require_negative_checks=True, require_readonly_inputs=True)
        delayed, delayed_qemu = readonly_fixture(recovery=True)
        for before, after in (("compute_pins=1 in_flight_pins=1", "compute_pins=3 in_flight_pins=3"),
                              ("compute_pins=2 in_flight_pins=2", "compute_pins=3 in_flight_pins=3")):
            with self.subTest(before=before), self.assertRaises(ValueError):
                validator.validate([delayed[0], delayed[1].replace(before, after)], delayed_qemu,
                                   True, 4, False, True)

    def test_readonly_cli_keeps_raw_source_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [pathlib.Path(directory) / name for name in ("a.log", "b.log", "q.log")]
            guests, qemu = readonly_fixture(negative=True)
            for path, data in zip(paths, guests + [qemu]):
                path.write_bytes(data.replace("\n", "\r\n").encode())
            command = [sys.executable, str(SCRIPT), "--node-a", str(paths[0]),
                       "--node-b", str(paths[1]), "--qemu-b", str(paths[2]),
                       "--require-readonly-inputs", "--require-negative-checks", "--access-threads", "4"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertTrue(report["readonly_inputs_verified"])
            for row, path in zip(report["sources"], paths):
                self.assertEqual(row["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

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
