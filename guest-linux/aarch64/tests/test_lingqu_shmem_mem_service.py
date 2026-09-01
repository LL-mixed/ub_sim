import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_ROOT = ROOT / "guest-linux" / "aarch64"
LIB_DIR = GUEST_ROOT / "libs" / "lingqu_shmem_pto"
ABI_DIR = ROOT / "crates" / "sim-qemu" / "include"
QEMU_UBC = ROOT / "vendor" / "qemu_8.2.0_ub" / "hw" / "ub" / "ub_ubc.c"
GOLDEN = GUEST_ROOT / "tests" / "lingqu_shmem_mem_service_golden.c"
MEM_SERVICE_PROFILE = ROOT / "mem_service" / "components" / "mem_service" / "mem_service_profile.h"
MEM_SERVICE_PUBLISH = (
    ROOT
    / "mem_service"
    / "components"
    / "mem_service"
    / "mem_service_model_range_publish_flow.c"
)


def _compile(compiler, output):
    subprocess.run(
        [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT),
            "-I",
            str(ROOT / "mem_service"),
            "-I",
            str(LIB_DIR),
            "-I",
            str(ABI_DIR),
            str(GOLDEN),
            str(LIB_DIR / "lingqu_shmem_mem_service.c"),
            str(LIB_DIR / "lingqu_shmem.c"),
            str(LIB_DIR / "lingqu_shmem_pto_guest.c"),
            "-o",
            str(output),
        ],
        check=True,
    )


class LingquShmemMemServiceTest(unittest.TestCase):
    def test_fake_backend_golden_covers_lease_and_wire_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "mem-service-golden"
            _compile("cc", binary)
            output = subprocess.check_output([str(binary)], text=True)
        self.assertEqual(output.strip(), "lingqu_shmem_mem_service_golden=pass")

    def test_obmm_backend_cross_compiles_without_warnings(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("aarch64 cross compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            for source_name in (
                "lingqu_shmem_mem_service.c",
                "lingqu_shmem_mem_service_obmm.c",
            ):
                output = pathlib.Path(directory) / f"{source_name}.o"
                subprocess.run(
                    [
                        compiler,
                        "-std=c11",
                        "-O2",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-D_GNU_SOURCE",
                        "-I",
                        str(ROOT),
                        "-I",
                        str(ROOT / "mem_service"),
                        "-I",
                        str(GUEST_ROOT),
                        "-I",
                        str(LIB_DIR),
                        "-I",
                        str(ABI_DIR),
                        "-D__EXPORTED_HEADERS__",
                        "-I",
                        str(ROOT / "vendor" / "obmm" / "src" / "libobmm"),
                        "-idirafter",
                        str(ROOT / "guest-linux" / "kernel_ub" / "include" / "uapi"),
                        "-idirafter",
                        str(ROOT / "guest-linux" / "kernel_ub" / "include"),
                        "-c",
                        str(LIB_DIR / source_name),
                        "-o",
                        str(output),
                    ],
                    check=True,
                )
                self.assertEqual(output.read_bytes()[:4], b"\x7fELF")

    def test_memory_service_in_place_publish_is_explicit_and_fail_closed(self):
        profile = MEM_SERVICE_PROFILE.read_text()
        publish = MEM_SERVICE_PUBLISH.read_text()

        self.assertIn("bool publish_payload_in_place;", profile)
        self.assertIn("uint64_t publish_payload_offset;", profile)
        self.assertIn("if (request->publish_payload_in_place)", publish)
        self.assertIn("payload != base + request->publish_payload_offset", publish)
        self.assertIn("runtime_output_in_place_invalid", publish)
        self.assertIn("if (!request->publish_payload_in_place)", publish)
        self.assertIn('" payload_mode=%s producer_publish_ms=', publish)

    def test_obmm_backend_uses_registered_map_local_pa(self):
        source = (LIB_DIR / "lingqu_shmem_mem_service_obmm.c").read_text()

        self.assertIn("ub_gm_addr = lease->map.local_pa;", source)
        self.assertIn("fallback_local_pa += runtime->payload_offset;", source)
        self.assertIn("lingqu_shmem_sim_phys_for_virt", source)

    def test_local_output_uses_self_import_without_payload_copy(self):
        source = (LIB_DIR / "lingqu_shmem_mem_service_obmm.c").read_text()
        qemu = QEMU_UBC.read_text()

        self.assertIn("obmm_compute_ensure_local_alias", source)
        self.assertIn("runtime->node_count, runtime->region_size", source)
        self.assertIn("context->local_alias_mem_id", source)
        self.assertIn("context->local_alias_pa + runtime->payload_offset", source)
        self.assertIn(".data = (uint8_t *)slot->region.addr + offset", source)
        self.assertNotIn("memcpy(", source)
        self.assertGreaterEqual(
            qemu.count("if (dcna == ubc_dev->parent.cna)"),
            2,
        )
        self.assertIn("ubc_dma_write_local_data_tid_strict(", qemu)
        self.assertIn("ubc_dma_read_local_data_tid_strict(", qemu)


if __name__ == "__main__":
    unittest.main()
