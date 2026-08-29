import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
APP_DIR = ROOT / "guest-linux" / "aarch64" / "apps" / "lingqu_shmem_pto_direct"
BUILD_INITRAMFS = ROOT / "guest-linux" / "aarch64" / "scripts" / "build_initramfs.sh"
RUN_APP = ROOT / "guest-linux" / "aarch64" / "initramfs" / "run_app"


class LingquShmemPtoDirectTest(unittest.TestCase):
    def test_cross_links_static_aarch64_cli(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("aarch64 cross compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "lingqu_shmem_pto_direct"
            subprocess.run(
                [
                    "make",
                    "-C",
                    str(APP_DIR),
                    f"CC={compiler}",
                    f"TARGET={binary}",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            data = binary.read_bytes()
        self.assertEqual(data[:4], b"\x7fELF")
        self.assertIn(b"LINGQU_SHMEM_PTO_RESULT", data)
        self.assertIn(b"--artifact-fingerprint", data)

    def test_workload_uses_public_memrefs_and_producer_verification(self):
        source = (APP_DIR / "lingqu_shmem_pto_direct.c").read_text()
        self.assertIn("lingqu_shmem_memref_create", source)
        self.assertIn("lingqu_shmem_pto_dispatch_prepare", source)
        self.assertIn("lingqu_shmem_pto_endpoint_submit", source)
        self.assertIn("obmm_async_map_register", source)
        self.assertIn("producer_verify=pass", source)
        self.assertNotIn("NPU_OP_PTO_DISPATCH", source)
        self.assertNotIn("MAP_GSVA", source)

    def test_initramfs_builds_and_installs_workload(self):
        build = BUILD_INITRAMFS.read_text()
        self.assertIn("LINGQU_SHMEM_PTO_DIRECT_SRC=", build)
        self.assertIn('"$LINGQU_SHMEM_PTO_DIRECT_SRC"', build)
        self.assertIn('"$LINGQU_SHMEM_PTO_DIRECT_WIRE_SRC"', build)
        self.assertIn('"$LINGQU_SHMEM_PTO_DIRECT_ENDPOINT_SRC"', build)
        self.assertIn(
            '"$INITRAMFS_DIR/bin/lingqu_shmem_pto_direct"', build
        )

    def test_run_app_maps_cmdline_to_workload_cli(self):
        run_app = RUN_APP.read_text()
        self.assertIn("run_lingqu_shmem_pto_direct()", run_app)
        self.assertIn("lingqu_shmem_pto_role", run_app)
        self.assertIn("lingqu_shmem_pto_requester_cna", run_app)
        self.assertIn("lingqu_shmem_pto_artifact_fingerprint", run_app)
        self.assertIn("linqu_shmem_pto_direct=1", run_app)
        self.assertIn("/bin/lingqu_shmem_pto_direct", run_app)


if __name__ == "__main__":
    unittest.main()
