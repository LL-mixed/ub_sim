#!/usr/bin/env python3
"""Validate endpoint resource baselines and live mappings in managed guest logs."""
import argparse
from collections import Counter
from pathlib import Path


def validate(text, node, iterations, size):
    expected = ([0, 1, 1, 0, 0] if node == 0 else [0, 1, 1, 0]) * iterations
    samples = []
    service_imports = None
    for line in text.splitlines():
        if "action=stats status=ok" in line:
            fields = dict(item.split("=", 1) for item in line.split() if "=" in item)
            service_imports = int(fields["import_mappings"])
        if not line.startswith("mem_service provider-resources:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate resource field")
            fields[key] = value
        if (fields.pop("provider"), fields.pop("scope"), fields.pop("version")) != (
                "obmm", "endpoint", "1"):
            raise ValueError("unsupported resource snapshot")
        values = {key: int(value) for key, value in fields.items()}
        live = values["accessible_views"]
        if service_imports is None or (live and service_imports < live):
            raise ValueError("live provider mapping absent from service ledger")
        required = {
            "export_handles": 1, "export_bytes": 2097152,
            # The home maps its existing export FD; only the peer imports.
            "import_handles": live * node, "import_bytes": live * node * 2097152,
            "vma_bytes": live * 2097152, "accessible_bytes": live * size,
            "cleanup_mappings": 0, "closing": 0, "control_close_uncertain": 0,
        }
        if live not in (0, 1) or any(values[key] != value for key, value in required.items()):
            raise ValueError("endpoint resources differ from expected ownership")
        if values["vma_count"] not in ((0,) if not live else (1, 2)):
            raise ValueError("unexpected view/guard VMA count")
        if node == 0 and len(samples) % 5 == 4 and service_imports != 0:
            raise ValueError("service mappings remain after object reclaim")
        samples.append(live)
        service_imports = None
    if samples != expected:
        raise ValueError(f"resource sequence mismatch: {samples}, expected {expected}")


def validate_kernel_regions(text):
    """Compare quiescent sysfs observations; this cannot prove VMA fencing."""
    observations = []
    phase = None
    regions = {}
    for line in text.splitlines():
        if line.startswith("mem_service kernel-regions:"):
            if phase is not None or len(observations) >= 2:
                raise ValueError("overlapping or extra kernel census")
            phase = line.removeprefix("mem_service kernel-regions: phase=")
            if phase != ("baseline", "drained")[len(observations)]:
                raise ValueError("unexpected kernel census phase")
            regions = {}
        elif line.startswith("obmm-kernel-region:"):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            region_id, size = int(fields["id"]), int(fields["bytes"])
            kind = fields["type"]
            if (phase is None or region_id < 0 or region_id in regions or
                    size <= 0 or kind not in ("export", "import")):
                raise ValueError("invalid or duplicate kernel region")
            regions[region_id] = (kind, size)
        elif line.startswith("obmm-kernel-regions:"):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            if (phase is None or fields["version"] != "1" or fields["scope"] != "sysfs" or
                    fields["consistency"] != "non_atomic"):
                raise ValueError("invalid kernel census summary")
            for kind in ("export", "import"):
                sizes = [size for typ, size in regions.values() if typ == kind]
                if (int(fields[f"{kind}_handles"]) != len(sizes) or
                        int(fields[f"{kind}_bytes"]) != sum(sizes)):
                    raise ValueError("kernel census summary differs from region records")
            observations.append(regions)
            phase = None
    if phase is not None or len(observations) != 2 or observations[0] != observations[1]:
        raise ValueError("kernel regions did not return to observed baseline")


def validate_capacity(text, node, iterations):
    """Require saturation with two real mapping lifetimes and a drained ledger."""
    if iterations != 1:
        raise ValueError("capacity profile requires one lifecycle")
    if node != 0:
        marker = ("action=wait_state key=obj-returned-1 status=ok "
                  "state=retired")
        if marker not in text:
            raise ValueError("peer did not observe released capacity barrier")
        return
    if text.count("mem_service capacity-pressure: phase=saturated ") != 1 or \
            text.count("mem_service capacity-pressure: phase=released status=ok") != 1:
        raise ValueError("capacity pressure did not complete exactly once")
    saturated_text, drained_text = text.split(
        "mem_service capacity-pressure: phase=saturated ", 1)[1].split(
        "mem_service capacity-pressure: phase=released status=ok", 1)

    def stats(block, last=False):
        snapshots = block.split("mem_service allocation-stats: status=ok\n")[1:]
        if not snapshots:
            raise ValueError("missing allocation capacity snapshot")
        fields = {}
        for line in (snapshots[-1] if last else snapshots[0]).splitlines():
            name, separator, value = line.partition("=")
            if separator and name in (
                "idempotency_capacity", "idempotency_used", "idempotency_cleanup_reserved",
                "idempotency_available", "idempotency_reservation_deficit",
                "import_mappings", "live_refs", "live_objects", "in_flight",
                "backing_allocated_bytes", "address_reserved_bytes",
            ):
                if name in fields:
                    raise ValueError("duplicate capacity field")
                fields[name] = int(value)
            if name == "idempotency_reservation_deficit":
                return fields
        raise ValueError("incomplete allocation capacity snapshot")

    saturated, drained = stats(saturated_text), stats(drained_text, last=True)
    if (saturated["import_mappings"] != 2 or saturated["live_refs"] != 2 or
            saturated["idempotency_cleanup_reserved"] != 9 or
            saturated["idempotency_used"] + saturated["idempotency_cleanup_reserved"] !=
            saturated["idempotency_capacity"] or
            saturated["idempotency_available"] != 0 or
            saturated["idempotency_reservation_deficit"] != 0):
        raise ValueError("capacity was not exhausted with both mappings protected")
    if any(drained[field] != 0 for field in (
            "import_mappings", "live_refs", "live_objects", "in_flight",
            "backing_allocated_bytes", "address_reserved_bytes",
            "idempotency_cleanup_reserved", "idempotency_available",
            "idempotency_reservation_deficit")) or \
            drained["idempotency_used"] != drained["idempotency_capacity"]:
        raise ValueError("full-table cleanup did not drain all resources")


def validate_sdk_contract(text, node, iterations):
    """Require per-object SDK rejections and actual holder-op replay results."""
    failures, successes = Counter(), Counter()
    completions = 0
    for line in text.splitlines():
        if not line.startswith("mem_service object-session:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate session result field")
            fields[key] = value
        if fields.get("session") != f"client-{node}":
            continue
        if fields.get("result") == "failed" or "mismatch" in fields:
            raise ValueError("managed SDK contract session failed")
        completions += fields.get("result") == "ok"
        if "action" not in fields:
            continue
        key, action, status = fields.get("key", "-"), fields["action"], fields["status"]
        if status == "ok":
            successes[key, action] += 1
        else:
            failures[key, action, status, fields.get("note", "")] += 1
    expected = Counter()
    for iteration in range(1, iterations + 1):
        key = f"obj-share-{iteration}"
        expected["absent-" + key, "acquire", "not_found", ""] = 1
        expected[key, "acquire", "stale_ref", ""] = 1
        expected[key, "write", "unsupported", "not_writable"] = 1
        expected[key, "release", "internal", "mapping_active"] = 1
        expected[key, "read", "checksum_mismatch", "pattern_mismatch"] = 1
        for action in ("read", "write", "publish_data", "wait_visible"):
            expected[key, action, "capacity_exceeded", "out_of_bounds"] = 1
        for action in ("acquire", "release"):
            if successes[key, action] != 2:
                raise ValueError("missing or extra holder operation replay")
    if failures != expected or completions != iterations:
        raise ValueError("managed SDK contract evidence differs from expected rounds")


def validate_map_conflict(text, node, iterations):
    """Require real EEXIST with a retained original mapping and exact remapping."""
    expected = {f"obj-share-{i}" for i in range(1, iterations + 1)}
    probes, mappings, results = Counter(), Counter(), Counter()
    addresses = {}
    pending = None
    completions = 0
    for line in text.splitlines():
        provider = line.startswith("mem_service obmm-map:")
        if not provider and not line.startswith("mem_service object-session:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate conflict evidence field")
            fields[key] = value
        if provider:
            if (pending is not None or fields["result"] != "failed" or
                    fields["stage"] != "mmap" or int(fields["errno"]) != 17):
                raise ValueError("expected exactly one actual mmap EEXIST per probe")
            pending = (int(fields["fixed_va"], 0), int(fields["len"]))
            continue
        if fields.get("session") != f"client-{node}":
            continue
        if (fields.get("result") == "failed" or "mismatch" in fields or
                fields.get("status", "ok") != "ok"):
            raise ValueError("conflict session failed")
        completions += fields.get("result") == "ok"
        key = fields.get("key")
        if "cpu_conflict_probe" in fields:
            base, page = int(fields["base"], 0), int(fields["len"])
            if (fields["cpu_conflict_probe"] != "pass" or key not in expected or
                    fields["preserved"] != "1" or fields["cleanup_pending"] != "0" or
                    fields.get("source") != "retained_handle" or
                    page not in (4096, 16384, 65536) or not base or base % page or
                    mappings[key] != 1 or addresses.get(key) != base or
                    pending is None or pending[0] != base or pending[1] < page):
                raise ValueError("conflict probe lacks matching preserved-page evidence")
            probes[key] += 1
            pending = None
        if fields.get("action") == "probe_conflict":
            results[key] += 1
        if fields.get("action") == "map":
            base = int(fields["base"], 0)
            if key not in expected or pending is not None:
                raise ValueError("unexpected mapping during conflict probe")
            if mappings[key] == 0:
                addresses[key] = base
            elif addresses[key] != base or probes[key] != 1:
                raise ValueError("remapping changed address or preceded conflict probe")
            mappings[key] += 1
    if (pending is not None or completions != iterations or
            probes != Counter({key: 1 for key in expected}) or results != probes or
            mappings != Counter({key: 2 for key in expected})):
        raise ValueError("missing or extra conflict probe/remapping evidence")


def validate_descriptor_contract(text, node, iterations):
    """Require each binding mutation to fail before the original view maps."""
    expected, failures, mappings = Counter(), Counter(), Counter()
    completions = 0
    for iteration in range(1, iterations + 1):
        key = f"obj-share-{iteration}"
        for byte in (0, 4, 8, 12, 16, 24, 32, 40, 44, 48, 56, 64, 68, 72,
                     76, 80, 84, 88, 92, 95):
            expected[key, "descriptor", str(byte)] = 1
        for fault in ("descriptor_length", "descriptor_oversize", "address", "address_len",
                      "size", "alignment", "capabilities", "home", "incarnation"):
            expected[key, fault, ""] = 1
    for line in text.splitlines():
        if line.startswith("mem_service obmm-map:"):
            raise ValueError("binding rejection unexpectedly reached provider mmap failure")
        if not line.startswith("mem_service object-session:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate binding evidence field")
            fields[key] = value
        if fields.get("session") != f"client-{node}":
            continue
        if fields.get("result") == "failed" or "mismatch" in fields:
            raise ValueError("descriptor contract session failed")
        completions += fields.get("result") == "ok"
        if "fault" in fields:
            key = fields.get("key", "")
            if (fields.get("action") != "map" or fields.get("status") != "stale_ref" or
                    fields.get("note") != "map_failed" or mappings[key]):
                raise ValueError("binding mutation did not fail before ordinary mapping")
            failures[key, fields["fault"], fields.get("fault_byte", "")] += 1
        elif fields.get("status", "ok") != "ok":
            raise ValueError("unexpected descriptor contract rejection")
        elif fields.get("action") == "map":
            mappings[fields.get("key", "")] += 1
    if (failures != expected or completions != iterations or
            mappings != Counter({f"obj-share-{i}": 2 for i in range(1, iterations + 1)})):
        raise ValueError("missing or extra descriptor binding evidence")


def pressure_snapshots(block, label, terminal):
    result = []
    for payload in block.split(f"mem_service {label}: status=ok\n")[1:]:
        fields = {}
        for line in payload.splitlines():
            key, sep, value = line.partition("=")
            if not sep or not key.replace("_", "").isalnum():
                continue
            if key in fields:
                raise ValueError("duplicate pressure field")
            fields[key] = value
            if key == terminal:
                result.append(fields)
                break
        else:
            raise ValueError("incomplete pressure snapshot")
    return result


def validate_address_capacity(text, node, iterations, size):
    """Check real reservations and refusal before/after non-reusable retirement."""
    if iterations != 1:
        raise ValueError("address capacity profile requires one lifecycle")
    if node:
        if "action=wait_state key=obj-returned-1 status=ok state=retired" not in text:
            raise ValueError("peer did not observe address pressure barrier")
        return
    markers = [f"mem_service address-pressure: phase={phase}" for phase in
               ("filled", "rejected", "released")]
    if any(text.count(marker) != 1 for marker in markers):
        raise ValueError("address pressure did not complete exactly once")
    positions = [text.index(marker) for marker in markers]
    if positions != sorted(positions):
        raise ValueError("out-of-order address pressure")
    filled = text[positions[0]:positions[1]]
    rejected = text[positions[1]:positions[2]]

    def objects(block, expected):
        views = pressure_snapshots(block, "inspect-allocation", "descriptor_hex")
        if len(views) != len(expected) or {v["key"] for v in views} != set(expected):
            raise ValueError("missing or extra allocation evidence")
        return {view["key"]: view for view in views}

    active = ("obj-share-1", "obj-sent-1", "obj-returned-1", "address-fill")
    early = ("address-overflow", "address-alignment")
    late = ("address-exhausted", "address-no-reuse")
    initial = objects(filled, active + early)
    final = objects(rejected, ("address-fill",) + late)
    ranges = []
    for key, logical, alignment in zip(active, (size, 65536, 65536, 8388608),
                                       (4194304, 0, 0, 8388608)):
        view = initial[key]
        address, length = int(view["address"]), int(view["address_len"])
        if (view["state"] != "active" or view["provider_backed"] != "1" or
                int(view["size_bytes"]) != logical or int(view["alignment_bytes"]) != alignment or
                length != (logical + 2097151) // 2097152 * 2097152 or
                address % max(alignment, 2097152) or
                address < 0x700000000000 or address + length > 0x700001000000 or
                int(view["descriptor_len"]) != 96 or len(view["descriptor_hex"]) != 192):
            raise ValueError("invalid actual allocation geometry")
        ranges.append((address, address + length))
    ordered = sorted(ranges)
    if any(left[1] > right[0] for left, right in zip(ordered, ordered[1:])) or \
            ranges[-1][1] != 0x700001000000:
        raise ValueError("overlapping reservations or aperture not exhausted")
    for view in [initial[key] for key in early] + list(final.values()):
        if (view["state"] != "retired" or view["descriptor_hex"] or
                any(int(view[field]) for field in
                    ("address", "address_len", "descriptor_len", "provider_backed", "live_refs"))):
            raise ValueError("rejected or retired allocation retained accessible resources")
    if initial["address-fill"]["generation"] != final["address-fill"]["generation"]:
        raise ValueError("filler identity changed during reclaim")
    expected_rejected = {initial[key]["generation"] for key in early} | \
                        {final[key]["generation"] for key in late}
    observed_rejected = []
    for line in text.splitlines():
        if line.startswith("obmm-worker ") and "reason=address_capacity" in line:
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            observed_rejected.append(fields["generation"])
    if len(observed_rejected) != 4 or set(observed_rejected) != expected_rejected:
        raise ValueError("missing worker capacity cancellations")
    for generation in expected_rejected:
        if f"obmm-worker generation={generation} phase=reserve-intent " in text:
            raise ValueError("capacity cancellation followed a resource-creating attempt")
    for block, backing, live in ((filled, 14680064, 4), (rejected, 6291456, 3)):
        stats = pressure_snapshots(block, "allocation-stats", "idempotency_reservation_deficit")
        if len(stats) != 1:
            raise ValueError("missing pressure resource counters")
        expected = {"backing_allocated_bytes": backing, "address_reserved_bytes": backing,
                    "live_objects": live, "export_mappings": live, "import_mappings": 2,
                    "live_refs": 2, "in_flight": 0, "quarantined_objects": 0,
                    "quarantined_bytes": 0, "idempotency_reservation_deficit": 0}
        if any(int(stats[0][key]) != value for key, value in expected.items()):
            raise ValueError("existing mappings or reservation accounting changed under pressure")


def validate_backing_capacity(text, node, iterations):
    """Require checked pool failure, pinned live mappings, and subsequent allocation."""
    if iterations != 1:
        raise ValueError("backing capacity profile requires one lifecycle")
    if node:
        if "action=wait_state key=obj-returned-1 status=ok state=retired" not in text:
            raise ValueError("peer did not observe backing pressure barrier")
        return
    phases = ("before", "rejected", "recovered", "drained", "complete")
    markers = [f"mem_service backing-pressure: phase={phase}" for phase in phases]
    if any(text.count(marker) != 1 for marker in markers):
        raise ValueError("backing pressure did not complete exactly once")
    positions = [text.index(marker) for marker in markers]
    if positions != sorted(positions):
        raise ValueError("out-of-order backing pressure")
    blocks = [text[start:end] for start, end in zip(positions, positions[1:])]
    pools = []
    for phase, block, live in zip(phases, blocks, (3, 3, 4, 3)):
        stats = pressure_snapshots(block, "allocation-stats", "idempotency_reservation_deficit")
        expected = {"backing_allocated_bytes": live * 2097152,
                    "address_reserved_bytes": live * 2097152, "live_objects": live,
                    "export_mappings": live, "import_mappings": 2, "live_refs": 2,
                    "in_flight": 0, "quarantined_objects": 0, "quarantined_bytes": 0,
                    "idempotency_reservation_deficit": 0}
        if len(stats) != 1 or any(int(stats[0][key]) != value for key, value in expected.items()):
            raise ValueError("existing mappings or service resources changed under backing pressure")
        pool_lines = [line for line in block.splitlines()
                      if line.startswith("mem_service backing-pool:")]
        if len(pool_lines) != 1:
            raise ValueError("missing or duplicate physical pool observation")
        pool = dict(item.split("=", 1) for item in pool_lines[0].split()[2:])
        if (pool["phase"] != phase or pool["consistency"] != "non_atomic" or
                not 0 <= int(pool["ready"]) <= int(pool["total"]) < 2147483648 or
                not 0 < int(pool["used"]) < 2147483648):
            raise ValueError("invalid physical pool observation")
        pools.append(int(pool["used"]))
    if pools != [pools[0], pools[0], pools[0] + 2097152, pools[0]]:
        raise ValueError("physical pool usage did not roll back exactly")
    views = []
    for block, key in zip(blocks[1:3], ("backing-empty", "backing-recovered")):
        found = pressure_snapshots(block, "inspect-allocation", "descriptor_hex")
        if len(found) != 1 or found[0]["key"] != key:
            raise ValueError("missing backing allocation identity")
        views.append(found[0])
    empty, recovered = views
    if (empty["state"] != "retired" or int(empty["size_bytes"]) != 2147483648 or
            empty["descriptor_hex"] or any(int(empty[key]) for key in
            ("address", "address_len", "descriptor_len", "provider_backed", "live_refs"))):
        raise ValueError("failed backing allocation retained resources")
    address = int(recovered["address"])
    if (recovered["state"] != "active" or recovered["provider_backed"] != "1" or
            recovered["generation"] == empty["generation"] or
            int(recovered["size_bytes"]) != 65536 or int(recovered["address_len"]) != 2097152 or
            int(recovered["descriptor_len"]) != 96 or len(recovered["descriptor_hex"]) != 192 or
            address % 2097152 or not 0x700080600000 <= address <= 0x7000ffe00000):
        raise ValueError("subsequent allocation missing or reused the retired address")
    records = [dict(item.split("=", 1) for item in line.split()[1:])
               for line in text.splitlines() if line.startswith("obmm-worker generation=")]
    failed = [record for record in records if record["generation"] == empty["generation"]]
    sequence = [record["phase"] for record in failed if "phase" in record]
    if sequence != ["reserve-intent", "reserved", "export-no-backing", "unbacked-retire-intent",
                    "unbacked-retired", "capacity-reject-intent", "cancel-empty", "cancel-confirmed"]:
        raise ValueError("missing explicit no-backing receipt or ordered confirmed rollback")
    reservations = [record for record in failed if record.get("phase") in
                    ("reserved", "export-no-backing", "unbacked-retire-intent", "unbacked-retired")]
    if (len({record["segment"] for record in reservations}) != 1 or
            int(reservations[0]["segment"]) <= 0 or
            any(int(record["export"]) for record in reservations) or
            len([r for r in failed if r.get("reason") == "backing_allocation" and
                 r.get("state") == "retired"]) != 1):
        raise ValueError("rollback used inconsistent kernel identity")
    pins = [line for line in text.splitlines() if line.startswith("mem_service backing-pin:")]
    if len(pins) != 1:
        raise ValueError("missing live segment retirement refusal")
    pin = dict(item.split("=", 1) for item in pins[0].split()[2:])
    if (pin["errno"] != "16" or int(pin["epoch"], 0) <= 0 or
            not any(record.get("phase") == "published" and int(record["export"]) > 0 and
                    int(record["segment"]) == int(pin["segment"], 0) for record in records)):
        raise ValueError("retirement refusal did not refer to a published live segment")
    expected_fields = {"version", "flags", "reserved"} | {
        "segment." + field for field in ("version", "flags", "segment_id", "home_va", "size",
            "epoch", "home_cna", "owner_node_id", "node_count", "cache_policy", "p_tag",
            "access_flags", "token_id", "token_value")}
    checked = []
    for line in text.splitlines():
        if not line.startswith("[gva_manager] export_identity "):
            continue
        fields = dict(item.split("=", 1) for item in line.split()[2:])
        expected_errno = 116 if fields["field"].startswith("segment.") else 22
        if (int(fields["errno"]) != expected_errno or fields["preserved"] != "1" or
                int(fields["segment"], 0) != int(pin["segment"], 0)):
            raise ValueError("kernel identity rejection did not preserve the held segment")
        checked.append(fields["field"])
    if (len(checked) != 17 or set(checked) != expected_fields or
            text.count("result=done action=gsva-export-identity-check checks=17 ") != 1):
        raise ValueError("missing independent kernel export identity checks")


def pressure_records(block, label):
    records = []
    for line in block.splitlines():
        if not line.startswith(f"mem_service {label}:"):
            continue
        fields = {}
        for item in line.split()[2:]:
            key, value = item.split("=", 1)
            if key in fields:
                raise ValueError("duplicate allocation pressure record field")
            fields[key] = value
        records.append(fields)
    return records


def validate_allocation_pressure(text, node, iterations, size):
    """Check overlapping client lifetimes, real interval identities, and reclaim."""
    if iterations != 1:
        raise ValueError("allocation pressure requires one lifecycle")
    if node:
        if "action=wait_state key=obj-returned-1 status=ok state=retired" not in text:
            raise ValueError("peer did not observe allocation pressure barrier")
        return
    phases = ("before", "live", "hole", "replaced", "drained", "complete")
    markers = [f"mem_service allocation-pressure: phase={phase}" for phase in phases]
    if any(text.count(marker) != 1 for marker in markers):
        raise ValueError("allocation pressure did not complete exactly once")
    positions = [text.index(marker) for marker in markers]
    if positions != sorted(positions):
        raise ValueError("out-of-order allocation pressure")
    blocks = [text[start:end] for start, end in zip(positions, positions[1:])]
    pools = []
    for phase, block, live, backing in zip(phases, blocks, (3, 6, 5, 6, 3),
                                           (6, 18, 14, 18, 6)):
        stats = pressure_snapshots(block, "allocation-stats", "idempotency_reservation_deficit")
        expected = {"backing_allocated_bytes": backing * 1048576,
                    "address_reserved_bytes": backing * 1048576,
                    "live_objects": live, "export_mappings": live, "import_mappings": 2,
                    "live_refs": 2, "in_flight": 0, "quarantined_objects": 0,
                    "quarantined_bytes": 0, "idempotency_reservation_deficit": 0}
        if len(stats) != 1 or any(int(stats[0][key]) != value for key, value in expected.items()):
            raise ValueError("allocation pressure changed held mappings or service accounting")
        pool = pressure_records(block, "allocation-pool")
        if (len(pool) != 1 or pool[0]["phase"] != phase or
                pool[0]["consistency"] != "non_atomic" or int(pool[0]["used"]) <= 0):
            raise ValueError("invalid allocation pool observation")
        pools.append(int(pool[0]["used"]))
    if [used - pools[0] for used in pools] != [0, 12582912, 8388608, 12582912, 0]:
        raise ValueError("physical backing usage did not follow actual allocations")

    original = ("obj-share-1", "obj-sent-1", "obj-returned-1")
    added = tuple(f"allocation-{i}" for i in range(1, 5))
    expected_keys = (original, added[:3], (added[1],), (added[3],), added)
    snapshots = []
    for block, keys in zip(blocks, expected_keys):
        found = pressure_snapshots(block, "inspect-allocation", "descriptor_hex")
        if len(found) != len(keys) or {view["key"] for view in found} != set(keys):
            raise ValueError("missing or extra allocation pressure object")
        snapshots.append({view["key"]: view for view in found})
    active = {**snapshots[0], **snapshots[1], **snapshots[3]}
    geometry = dict(zip(original + added,
                        ((size, 0), (65536, 0), (65536, 0), (1, 2097152),
                         (2097153, 4194304), (4194305, 8388608), (3145728, 4194304))))
    ranges, bindings = {}, {}
    for key, view in active.items():
        logical, alignment = geometry[key]
        address, length = int(view["address"]), int(view["address_len"])
        descriptor = bytes.fromhex(view["descriptor_hex"])
        if (view["state"] != "active" or view["provider_backed"] != "1" or
                view["home_node"] != "guest-node-0" or int(view["provider_incarnation"]) <= 0 or
                int(view["size_bytes"]) != logical or int(view["alignment_bytes"]) != alignment or
                length != (logical + 2097151) // 2097152 * 2097152 or
                address % max(alignment, 2097152) or address < 0x700000000000 or
                address + length > 0x700004000000 or int(view["generation"]) <= 0 or
                int(view["descriptor_len"]) != 96 or len(descriptor) != 96):
            raise ValueError("invalid allocation pressure geometry or identity")
        export, uba, backing = (int.from_bytes(descriptor[n:n + 8], "big") for n in (16, 24, 32))
        segment, epoch = (int.from_bytes(descriptor[n:n + 8], "big") for n in (48, 56))
        if min(export, segment, epoch) <= 0 or (uba, backing) != (address, length):
            raise ValueError("descriptor differs from allocation geometry")
        bindings[key] = (segment, epoch, export)
        ranges[key] = (address, address + length)
    if len({view["generation"] for view in active.values()}) != len(active) or \
            len({binding[0] for binding in bindings.values()}) != len(active):
        raise ValueError("distinct allocations share generation or segment identity")
    ordered = sorted(ranges.values())
    if (any(left[1] > right[0] for left, right in zip(ordered, ordered[1:])) or
            ranges[added[3]][0] < max(ranges[key][1] for key in original + added[:3])):
        raise ValueError("overlapping ranges or premature retired-address reuse")
    for block, keys in ((blocks[0], original), (blocks[1], added[:3]), (blocks[3], added[3:])):
        queried = pressure_records(block, "allocation-segment")
        if len(queried) != len(keys) or {record["key"] for record in queried} != set(keys):
            raise ValueError("missing independent kernel segment query")
        for record in queried:
            key = record["key"]
            actual = tuple(int(record[field], 0) for field in ("segment", "epoch", "address", "size"))
            expected = bindings[key][:2] + (ranges[key][0], ranges[key][1] - ranges[key][0])
            if actual != expected:
                raise ValueError("kernel query differs from published segment")
    for view in list(snapshots[2].values()) + list(snapshots[4].values()):
        if (view["state"] != "retired" or view["descriptor_hex"] or
                view["generation"] != active[view["key"]]["generation"] or
                any(int(view[field]) for field in
                    ("address", "address_len", "descriptor_len", "provider_backed", "live_refs"))):
            raise ValueError("retired pressure allocation retained resources or changed identity")
    journals = [dict(item.split("=", 1) for item in line.split()[1:])
                for line in text.splitlines() if line.startswith("obmm-worker generation=")]
    for key in added:
        records = [r for r in journals if r["generation"] == active[key]["generation"] and "phase" in r]
        if [r["phase"] for r in records] != ["reserve-intent", "reserved", "exported", "published",
                                            "release-intent", "unexported", "retired", "reclaimed"]:
            raise ValueError("allocation did not reserve and reclaim exactly once")
        segment, _, export = bindings[key]
        if any(int(r["segment"]) != segment for r in records[1:]) or \
                any(int(r["export"]) != export for r in records[2:6]):
            raise ValueError("worker reclaimed a different backing or segment")
    waiting = pressure_records(blocks[1], "allocation-client")
    exited = pressure_records(blocks[4], "allocation-client")
    if (len(waiting) != 3 or len(exited) != 3 or
            {r["index"] for r in waiting} != {"1", "2", "3"} or
            len({r["pid"] for r in waiting}) != 3 or
            any(r["phase"] != "waiting" or int(r["pid"]) <= 1 for r in waiting) or
            any(r["phase"] != "exited" or r["code"] != "0" for r in exited) or
            {(r["index"], r["pid"]) for r in waiting} !=
            {(r["index"], r["pid"]) for r in exited}):
        raise ValueError("missing concurrent client lifetimes or successful exits")
    sessions = pressure_records(blocks[4], "object-session")
    for index, key in enumerate(added[:3], 1):
        records = [r for r in sessions if r.get("session") == f"allocation-client-{index}"]
        operations = [r for r in records if "action" in r]
        finished = [r for r in records if "result" in r]
        if ([r["action"] for r in operations] != ["allocate", "wait_state", "allocate", "wait_state"] or
                len(finished) != 1 or finished[0]["result"] != "ok" or finished[0]["ops"] != "4" or
                any(r["status"] != "ok" or r["key"] != key or "mismatch" in r or
                    r["generation"] != active[key]["generation"] for r in operations) or
                operations[0]["state"] != operations[2]["state"] or
                operations[1]["state"] != "active" or operations[3]["state"] != "retired"):
            raise ValueError("concurrent allocation replay changed identity or lifecycle")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--node", type=int, choices=(0, 1), required=True)
    parser.add_argument("--iterations", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    faults = parser.add_mutually_exclusive_group()
    faults.add_argument("--sdk-contract", action="store_true")
    faults.add_argument("--map-conflict", action="store_true")
    faults.add_argument("--descriptor-contract", action="store_true")
    faults.add_argument("--capacity", action="store_true")
    faults.add_argument("--address-capacity", action="store_true")
    faults.add_argument("--backing-capacity", action="store_true")
    faults.add_argument("--allocation-pressure", action="store_true")
    args = parser.parse_args()
    if args.iterations < 1 or not 0 < args.size <= 2097152:
        parser.error("positive iterations and 1..2097152 logical bytes required")
    try:
        text = args.log.read_text()
        validate(text, args.node, args.iterations, args.size)
        validate_kernel_regions(text)
        if args.sdk_contract:
            validate_sdk_contract(text, args.node, args.iterations)
        if args.map_conflict:
            validate_map_conflict(text, args.node, args.iterations)
        if args.descriptor_contract:
            validate_descriptor_contract(text, args.node, args.iterations)
        if args.capacity:
            validate_capacity(text, args.node, args.iterations)
        if args.address_capacity:
            validate_address_capacity(text, args.node, args.iterations, args.size)
        if args.backing_capacity:
            validate_backing_capacity(text, args.node, args.iterations)
        if args.allocation_pressure:
            validate_allocation_pressure(text, args.node, args.iterations, args.size)
    except (OSError, KeyError, ValueError) as error:
        parser.exit(1, f"mapping resource validation failed: {error}\n")
    print(f"mapping_resources=pass node={args.node} iterations={args.iterations}")
    print(f"kernel_regions=pass node={args.node} consistency=non_atomic")
    if args.sdk_contract:
        print(f"sdk_contract=pass node={args.node} iterations={args.iterations}")
    if args.map_conflict:
        print(f"map_conflict=pass node={args.node} iterations={args.iterations}")
    if args.descriptor_contract:
        print(f"descriptor_contract=pass node={args.node} iterations={args.iterations}")
    if args.capacity:
        print(f"capacity_cleanup=pass node={args.node} iterations={args.iterations}")
    if args.address_capacity:
        print(f"address_capacity=pass node={args.node} iterations={args.iterations}")
    if args.backing_capacity:
        print(f"backing_capacity=pass node={args.node} iterations={args.iterations}")
    if args.allocation_pressure:
        print(f"allocation_pressure=pass node={args.node} iterations={args.iterations}")


if __name__ == "__main__":
    main()
