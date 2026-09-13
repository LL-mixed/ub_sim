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


class MappingResourceValidationTests(unittest.TestCase):
    def test_address_capacity_requires_actual_geometry_and_preserved_mappings(self):
        def view(key, generation, size, alignment=0, address=0, length=0):
            fields = {"key": key, "generation": generation, "size_bytes": size,
                      "alignment_bytes": alignment, "address": address, "address_len": length,
                      "provider_backed": int(bool(address)), "live_refs": 0,
                      "state": "active" if address else "retired",
                      "descriptor_len": 96 if address else 0,
                      "descriptor_hex": "ab" * 96 if address else ""}
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
                ("address-fill", 6, 8388608, 8388608, 8388608, 8388608)):
            text += view(key, generation, size, alignment, base + offset, length)
        text += view("address-overflow", 4, 2**64 - 1)
        text += view("address-alignment", 5, 1, 2**63)
        text += stats(14680064, 4)
        text += "mem_service address-pressure: phase=rejected\n"
        text += view("address-fill", 6, 8388608, 8388608)
        text += view("address-exhausted", 7, 1)
        text += view("address-no-reuse", 8, 1)
        text += stats(6291456, 3)
        text += "mem_service address-pressure: phase=released\n"
        for generation in (4, 5, 7, 8):
            text += f"obmm-worker generation={generation} state=retired reason=address_capacity\n"
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
