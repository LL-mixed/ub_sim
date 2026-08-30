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
        self.assertIn(b"--fault-case", data)

    def test_workload_uses_public_memrefs_and_producer_verification(self):
        source = (APP_DIR / "lingqu_shmem_pto_direct.c").read_text()
        self.assertIn("lingqu_shmem_memref_create", source)
        self.assertIn("lingqu_shmem_pto_dispatch_prepare", source)
        self.assertIn("lingqu_shmem_pto_endpoint_submit", source)
        self.assertIn("lingqu_shmem_pto_endpoint_submit_cancel_after", source)
        self.assertIn("obmm_async_map_register", source)
        self.assertIn("producer_verify=pass", source)
        self.assertIn("(sum + 1.0f) * (sum + 2.0f)", source)
        self.assertIn("msync_unsupported=1", source)
        self.assertIn("errno != EINVAL", source)
        self.assertIn(".ub_gm_addr = local_pas[0]", source)
        self.assertNotIn(
            "lingqu_shmem_sim_phys_for_virt(imported.addr", source
        )
        self.assertIn("PTO_DIRECT_HOST_VECTOR_ELEMENTS", source)
        self.assertIn("--expect OUTCOME", source)
        self.assertIn("PTO_DIRECT_EXPECT_AUTHORIZATION_TIMEOUT", source)
        self.assertIn("PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED", source)
        self.assertIn("--cancel-after-ms N", source)
        self.assertIn("output_is_sentinel", source)
        self.assertIn("output_unchanged=1", source)
        self.assertIn("expectation_error_code(config->expectation)", source)
        self.assertIn("LINGQU_PTO_UB_GM_CODE_AUTHORIZATION_CANCELLED", source)
        self.assertIn("PTO_DIRECT_FAULT_BAD_MAPPING_REF", source)
        self.assertIn("PTO_DIRECT_FAULT_STALE_MAPPING", source)
        self.assertIn("PTO_DIRECT_FAULT_RELEASED_IMPORT", source)
        self.assertIn("PTO_DIRECT_FAULT_WRONG_REQUESTER", source)
        self.assertIn("PTO_DIRECT_FAULT_OOB", source)
        self.assertIn("PTO_DIRECT_FAULT_ADDRESS_OVERFLOW", source)
        self.assertIn("PTO_DIRECT_FAULT_ROLE_ACCESS_MISMATCH", source)
        self.assertIn("PTO_DIRECT_FAULT_TSTORE_ON_READ", source)
        self.assertIn("PTO_DIRECT_FAULT_TLOAD_ON_WRITE", source)
        self.assertIn("refresh_fault_metadata_crc", source)
        self.assertIn("stage=fault_injected", source)
        self.assertIn("stage=fault_selected", source)
        self.assertIn("source=callable-artifact", source)
        self.assertNotIn(
            "wire_memrefs[2].role = LINGQU_PTO_MEMREF_INPUT", source
        )
        self.assertNotIn(
            "wire_memrefs[0].role = LINGQU_PTO_MEMREF_OUTPUT", source
        )
        self.assertIn(
            "config->elements != PTO_DIRECT_HOST_VECTOR_ELEMENTS", source
        )
        self.assertNotIn("NPU_OP_PTO_DISPATCH", source)
        self.assertNotIn("MAP_GSVA", source)
        self.assertIn("obmm_do_import(", source)
        self.assertIn("obmm_do_unimport(obmm_fd, released_mem_id)", source)
        self.assertIn("import_active=0", source)
        self.assertNotIn("obmm_do_import_v2(", source)
        self.assertNotIn("OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA", source)

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
        self.assertIn("lingqu_shmem_pto_expect success", run_app)
        self.assertIn("--expect $(cmdline_value", run_app)
        self.assertIn("lingqu_shmem_pto_cancel_after_ms 0", run_app)
        self.assertIn("lingqu_shmem_pto_fault_case none", run_app)
        self.assertIn("--fault-case $(cmdline_value", run_app)
        self.assertIn("linqu_shmem_pto_direct=1", run_app)
        self.assertIn("/bin/lingqu_shmem_pto_direct", run_app)
        self.assertIn("lingqu_shmem_pto_elements 16384", run_app)

    def test_dual_node_runner_wires_pto_identity_and_evidence_gates(self):
        runner = DUAL_NODE_RUNNER.read_text()
        self.assertIn("lingqu_shmem_pto_direct)", runner)
        self.assertIn('flag="linqu_shmem_pto_direct=1"', runner)
        self.assertIn("--pto-artifact-fingerprint", runner)
        self.assertIn("ubc.pto-device-cna=$LINGQU_SHMEM_PTO_NODEB_CNA", runner)
        self.assertIn("--pto-authorization-delay-ns", runner)
        self.assertIn("--pto-authorization-timeout-ns", runner)
        self.assertIn("--pto-cancel-after-ms", runner)
        self.assertIn("--pto-reset-on-pending", runner)
        self.assertIn("--pto-inject-duplicate-completion", runner)
        self.assertIn("--pto-inject-late-completion", runner)
        self.assertIn("--pto-expect", runner)
        self.assertIn("--pto-fault-case", runner)
        self.assertIn("LINGQU_SHMEM_PTO_EXPECT", runner)
        self.assertIn("LINGQU_SHMEM_PTO_FAULT_CASE", runner)
        self.assertIn(
            "lingqu_shmem_pto_expect=$LINGQU_SHMEM_PTO_EXPECT", runner
        )
        self.assertIn("status=pass expected=authorization-timeout", runner)
        self.assertIn(
            "ubc.pto-authorization-delay-ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS",
            runner,
        )
        self.assertIn(
            "ubc.pto-authorization-timeout-ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS",
            runner,
        )
        self.assertIn("LINGQU_SHMEM_PTO_RESULT role=producer status=pass", runner)
        self.assertIn("LINGQU_SHMEM_PTO_RESULT role=consumer status=pass", runner)
        self.assertIn("QEMU_UB_GM_AUTHORIZATION_PENDING", runner)
        self.assertIn("QEMU_UB_GM_AUTHORIZATION_RESUME", runner)
        self.assertIn("QEMU_UB_GM_AUTHORIZATION_TIMEOUT", runner)
        self.assertIn("pto_ub_gm_authorization_timeout", runner)
        self.assertIn("pto_ub_gm_authorization_cancelled", runner)
        self.assertIn("pto_ub_gm_bad_memref", runner)
        self.assertIn("pto_ub_gm_access_denied", runner)
        self.assertIn("QEMU_UB_GM_AUTHORIZATION_CANCEL", runner)
        self.assertIn("QEMU_UB_GM_RESET authorization_pending=1", runner)
        self.assertIn("source=cancel-late-injection reason=no_pending", runner)
        self.assertIn("source=reset-late-injection reason=no_pending", runner)
        self.assertIn("source=duplicate-injection reason=already_completed", runner)
        self.assertIn("qmp_execute_strict", runner)
        self.assertIn("system_reset", runner)
        self.assertIn("validate_lingqu_shmem_pto_reset_sequence", runner)
        self.assertIn("resumed_sequence <= reset_sequence", runner)
        self.assertIn("consumer exact-once timeout CQ completion", runner)
        self.assertIn("consumer retained CMDQ head while pending", runner)
        self.assertIn("consumer data access after authorization timeout", runner)
        self.assertIn("QEMU_UB_GM_INPUT_AUTHORIZE", runner)
        self.assertIn("QEMU_UB_GM_LOAD", runner)
        self.assertIn("QEMU_UB_GM_STORE", runner)
        self.assertIn("QEMU_UB_GM_FENCE", runner)
        self.assertIn("consumer execution-fault binding registration", runner)
        self.assertIn("consumer valid loads before denied PTO access", runner)
        self.assertIn("consumer exact-once released import fault", runner)
        self.assertIn("consumer released SIM_DEC import mapping", runner)
        self.assertIn("consumer SIM_DEC import unmap", runner)
        self.assertIn(
            "consumer exact-once requested callable access fault", runner
        )
        self.assertIn("source=callable-artifact", runner)
        self.assertIn(
            "consumer wire mutation for callable access fault", runner
        )
        self.assertIn("reason=completion_failure", runner)
        self.assertIn("segment_payload_staging_bytes=0", runner)
        self.assertIn("experimental NPU/GVA/GSVA leakage", runner)
        self.assertIn("UB_NPU: created|SIM_DEC: GVA_MAP", runner)
        self.assertIn("manifest UB_GM access fault mismatch", PTO_RUNNER.read_text())
        self.assertIn("manifest_ub_gm_access_fault", PTO_RUNNER.read_text())
        self.assertIn("prepare_simpler_host_artifacts.py", PTO_RUNNER.read_text())

    def test_dedicated_runner_derives_fingerprint_and_preserves_evidence(self):
        runner = PTO_RUNNER.read_text()
        self.assertIn("--fingerprint-manifest", runner)
        self.assertIn('payload.get("callable_id") != 1', runner)
        self.assertIn("evidence directory already exists", runner)
        self.assertIn("callable-fingerprint.json", runner)
        self.assertIn("artifact-paths.txt", runner)
        self.assertIn("source-sha256.txt", runner)
        self.assertIn(
            "apps/lingqu_shmem_pto_direct/lingqu_shmem_pto_direct.c",
            runner,
        )
        self.assertIn("initramfs/run_app", runner)
        self.assertIn("qemu-system-aarch64", runner)
        self.assertIn("qemu-leftovers.txt", runner)
        self.assertIn("pgrep -af '[q]emu-system-aarch64'", runner)
        self.assertNotIn("pgrep -af qemu-system-aarch64", runner)
        self.assertIn("validation.status=pass", runner)
        self.assertIn('QMP_ARGS=(--use-qmp)', runner)
        self.assertIn('--pto-reset-on-pending "$RESET_ON_PENDING"', runner)
        self.assertIn("reset_on_pending=$RESET_ON_PENDING", runner)
        result = subprocess.run(
            [str(PTO_RUNNER), "--help"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertIn("--manifest PATH", result.stdout)
        self.assertIn("--authorization-delay-ns N", result.stdout)
        self.assertIn("--authorization-timeout-ns N", result.stdout)
        self.assertIn("--cancel-after-ms N", result.stdout)
        self.assertIn("--reset-on-pending 0|1", result.stdout)
        self.assertIn("--inject-duplicate-completion 0|1", result.stdout)
        self.assertIn("--inject-late-completion 0|1", result.stdout)
        self.assertIn("--expect OUTCOME", result.stdout)
        self.assertIn("--fault-case CASE", result.stdout)

    def test_dedicated_runner_rejects_unknown_expected_result(self):
        result = subprocess.run(
            [
                str(PTO_RUNNER),
                "--manifest",
                "/does/not/need/to/exist",
                "--expect",
                "silent-fallback",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "expected result must be success, authorization-timeout, authorization-cancelled, bad-memref, or access-denied",
            result.stdout,
        )

    def test_dedicated_runner_rejects_unknown_fault_case(self):
        result = subprocess.run(
            [
                str(PTO_RUNNER),
                "--manifest",
                "/does/not/need/to/exist",
                "--fault-case",
                "invented-corruption",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "unsupported PTO fault case: invented-corruption",
            result.stdout,
        )

    def test_generic_runner_rejects_fault_expectation_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = root / "manifest.json"
            scenario = root / "scenario.yaml"
            manifest.write_text("{}\n")
            scenario.write_text("schema_version: 1\n")
            result = subprocess.run(
                [
                    str(DUAL_NODE_RUNNER),
                    "--app",
                    "lingqu_shmem_pto_direct",
                    "--pto-manifest",
                    str(manifest),
                    "--pto-scenario",
                    str(scenario),
                    "--pto-artifact-fingerprint",
                    "1",
                    "--pto-fault-case",
                    "wrong-requester",
                    "--pto-expect",
                    "bad-memref",
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "PTO fault case wrong-requester requires expected result access-denied",
            result.stdout,
        )

    def test_dedicated_runner_rejects_execution_fault_expectation_mismatch(self):
        result = subprocess.run(
            [
                str(PTO_RUNNER),
                "--manifest",
                "/does/not/need/to/exist",
                "--fault-case",
                "tstore-on-read",
                "--expect",
                "bad-memref",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "fault case tstore-on-read requires expected result access-denied",
            result.stdout,
        )

    def test_dedicated_runner_rejects_released_import_expectation_mismatch(self):
        result = subprocess.run(
            [
                str(PTO_RUNNER),
                "--manifest",
                "/does/not/need/to/exist",
                "--fault-case",
                "released-import",
                "--expect",
                "access-denied",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "fault case released-import requires expected result bad-memref",
            result.stdout,
        )

    def test_generic_runner_rejects_released_import_expectation_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = root / "manifest.json"
            scenario = root / "scenario.yaml"
            manifest.write_text("{}\n")
            scenario.write_text("schema_version: 1\n")
            result = subprocess.run(
                [
                    str(DUAL_NODE_RUNNER),
                    "--app",
                    "lingqu_shmem_pto_direct",
                    "--pto-manifest",
                    str(manifest),
                    "--pto-scenario",
                    str(scenario),
                    "--pto-artifact-fingerprint",
                    "1",
                    "--pto-fault-case",
                    "released-import",
                    "--pto-expect",
                    "access-denied",
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "PTO fault case released-import requires expected result bad-memref",
            result.stdout,
        )

    def test_generic_runner_rejects_reset_without_delayed_authorization(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = root / "manifest.json"
            scenario = root / "scenario.yaml"
            manifest.write_text("{}\n")
            scenario.write_text("schema_version: 1\n")
            result = subprocess.run(
                [
                    str(DUAL_NODE_RUNNER),
                    "--app",
                    "lingqu_shmem_pto_direct",
                    "--pto-manifest",
                    str(manifest),
                    "--pto-scenario",
                    str(scenario),
                    "--pto-artifact-fingerprint",
                    "1",
                    "--pto-authorization-delay-ns",
                    "0",
                    "--pto-reset-on-pending",
                    "1",
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(
            "reset-on-pending requires delayed successful authorization",
            result.stdout,
        )

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
                    "--authorization-delay-ns",
                    "250000",
                    "--authorization-timeout-ns",
                    "10000000",
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
            validation = (evidence / "validation.status").read_text()
            self.assertIn("authorization_delay_ns=250000", validation)
            self.assertIn("authorization_timeout_ns=10000000", validation)
            self.assertIn("cancel_after_ms=0", validation)
            self.assertIn("reset_on_pending=0", validation)
            self.assertIn("inject_duplicate_completion=0", validation)
            self.assertIn("inject_late_completion=0", validation)
            self.assertIn("expected_result=success", validation)
            self.assertIn("fault_case=none", validation)
            self.assertIn("qemu_binary=", validation)
            self.assertTrue((evidence / "source-sha256.txt").is_file())


if __name__ == "__main__":
    unittest.main()
