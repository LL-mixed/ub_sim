import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
APP_DIR = ROOT / "guest-linux" / "aarch64" / "apps" / "lingqu_shmem_pto_direct"
BUILD_INITRAMFS = ROOT / "guest-linux" / "aarch64" / "scripts" / "build_initramfs.sh"
RUN_APP = ROOT / "guest-linux" / "aarch64" / "initramfs" / "run_app"
DUAL_NODE_RUNNER = (
    ROOT / "guest-linux" / "aarch64" / "scripts" / "run_ub_dual_node_apps.sh"
)
PTO_RUNNER = (
    ROOT
    / "guest-linux"
    / "aarch64"
    / "scripts"
    / "run_ub_dual_node_lingqu_shmem_pto_direct.sh"
)


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
        self.assertIn("msync_unsupported=1", source)
        self.assertIn("errno != EINVAL", source)
        self.assertIn(".ub_gm_addr = local_pas[0]", source)
        self.assertNotIn(
            "lingqu_shmem_sim_phys_for_virt(imported.addr", source
        )
        self.assertIn("PTO_DIRECT_HOST_VECTOR_ELEMENTS", source)
        self.assertIn(
            "config->elements != PTO_DIRECT_HOST_VECTOR_ELEMENTS", source
        )
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
        self.assertIn("lingqu_shmem_pto_elements 16384", run_app)

    def test_dual_node_runner_wires_pto_identity_and_evidence_gates(self):
        runner = DUAL_NODE_RUNNER.read_text()
        self.assertIn("lingqu_shmem_pto_direct)", runner)
        self.assertIn('flag="linqu_shmem_pto_direct=1"', runner)
        self.assertIn("--pto-artifact-fingerprint", runner)
        self.assertIn("ubc.pto-device-cna=$LINGQU_SHMEM_PTO_NODEB_CNA", runner)
        self.assertIn("LINGQU_SHMEM_PTO_RESULT role=producer status=pass", runner)
        self.assertIn("LINGQU_SHMEM_PTO_RESULT role=consumer status=pass", runner)
        self.assertIn("QEMU_UB_GM_INPUT_AUTHORIZE", runner)
        self.assertIn("QEMU_UB_GM_LOAD", runner)
        self.assertIn("QEMU_UB_GM_STORE", runner)
        self.assertIn("QEMU_UB_GM_FENCE", runner)
        self.assertIn("segment_payload_staging_bytes=0", runner)

    def test_dedicated_runner_derives_fingerprint_and_preserves_evidence(self):
        runner = PTO_RUNNER.read_text()
        self.assertIn("--fingerprint-manifest", runner)
        self.assertIn('payload.get("callable_id") != 1', runner)
        self.assertIn("evidence directory already exists", runner)
        self.assertIn("callable-fingerprint.json", runner)
        self.assertIn("artifact-paths.txt", runner)
        self.assertIn("qemu-leftovers.txt", runner)
        self.assertIn("validation.status=pass", runner)
        result = subprocess.run(
            [str(PTO_RUNNER), "--help"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertIn("--manifest PATH", result.stdout)

    def test_dedicated_runner_preserves_command_path_during_canonicalization(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            artifacts = []
            for name in (
                "runtime_host.so",
                "orchestration.so",
                "runtime_aicpu.bin",
                "runtime_aicore.bin",
                "kernel_0.bin",
            ):
                artifact = root / name
                artifact.write_bytes(name.encode("ascii"))
                artifacts.append(artifact)

            manifest = root / "host_vector_manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "simpler_runtime": {
                            "host_runtime_library": {
                                "source": str(artifacts[0])
                            },
                            "orch_shared_object": {
                                "source": str(artifacts[1])
                            },
                            "aicpu_binary": {"source": str(artifacts[2])},
                            "aicore_binary": {"source": str(artifacts[3])},
                            "kernels": [
                                {"binary": {"source": str(artifacts[4])}}
                            ],
                        }
                    }
                )
            )
            scenario = root / "scenario.yaml"
            scenario.write_text("schema_version: 1\n")
            kernel = root / "Image"
            kernel.write_bytes(b"kernel")
            initramfs = root / "initramfs.cpio.gz"
            initramfs.write_bytes(b"initramfs")
            sim_cli = root / "sim-cli"
            sim_cli.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' '{\"command\":\"lingqu-shmem-pto-e2e\","
                "\"implementation_phase\":\"p3_guest_runtime\","
                "\"callable_id\":1,\"artifact_fingerprint\":1,"
                "\"artifact_fingerprint_hex\":\"0x0000000000000001\"}'\n"
            )
            sim_cli.chmod(0o755)
            evidence = root / "evidence"

            result = subprocess.run(
                [
                    str(PTO_RUNNER),
                    "--manifest",
                    str(manifest),
                    "--scenario",
                    str(scenario),
                    "--sim-cli-bin",
                    str(sim_cli),
                    "--kernel-image",
                    str(kernel),
                    "--initramfs-image",
                    str(initramfs),
                    "--elements",
                    "1",
                    "--run-id",
                    "canonical-path-contract",
                    "--evidence-dir",
                    str(evidence),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )

            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertNotIn("command not found", result.stdout)
            self.assertNotIn("permission denied", result.stdout)
            self.assertIn("PTO callable 1 requires exactly 16384", result.stdout)
            self.assertTrue((evidence / "sha256.txt").is_file())
            self.assertIn(
                "validation.status=fail",
                (evidence / "validation.status").read_text(),
            )


if __name__ == "__main__":
    unittest.main()
