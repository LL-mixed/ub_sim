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
