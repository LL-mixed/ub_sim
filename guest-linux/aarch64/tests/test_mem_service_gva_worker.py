import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SERVICE = ROOT / "mem_service"
COMPONENT = SERVICE / "components" / "mem_service"
PROVIDERS = COMPONENT / "providers"
MANAGER = ROOT / "guest-linux" / "aarch64" / "libs" / "gva_manager"


class MemServiceGvaWorkerTest(unittest.TestCase):
    def test_managed_guest_path_uses_worker_owned_backing(self):
        source = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        managed = source.split("mem_service_object_share_managed() (", 1)[1].split(
            "\nrun_mem_service_object_share()", 1)[0]
        for required in ("ip link set lo up", "serve-allocations", "--bootstrap",
                         "allocation_granularity_bytes=2097152",
                         "provider_import_region_bytes=2097152",
                         "op=publish_data", "op=wait_visible", "op=release",
                         "op=retire", "state=retired", 'wait "$worker_pid"'):
            self.assertIn(required, managed)
        for forbidden in ("descriptor_hex=", "op=publish key=", "op=reclaim key=",
                          "llm_infer", "put-object", "materialize-object"):
            self.assertNotIn(forbidden, managed)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_share_runner_rejects_missing_config_before_launch(self):
        runner = ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh"
        with tempfile.TemporaryDirectory() as directory:
            for args in (["--config"], ["--config", str(pathlib.Path(directory) / "missing")]):
                result = subprocess.run(["zsh", str(runner)] + args,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2)
                self.assertIn("config file not found", result.stderr)
                self.assertNotIn("starting node", result.stdout)

    @unittest.skipUnless(platform.system() == "Linux", "native Linux build test")
    def test_build_option_invalidates_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            app = root / "apps" / "mem_service"
            app.mkdir(parents=True)
            shutil.copyfile(SERVICE / "apps/mem_service/Makefile", app / "Makefile")
            for name in ("components", "common", "libs", "vendor", "kernel_ub"):
                (root / name).symlink_to(SERVICE / name, target_is_directory=True)
            for enabled in (True, False, True):
                built = subprocess.run(
                    ["make", "-s", "-C", str(app),
                     f"GVA_MANAGER_ROOT={MANAGER if enabled else ''}",
                     "linqu_mem_service_provider_obmm"],
                    capture_output=True, text=True, timeout=120,
                )
                self.assertEqual(built.returncode, 0, built.stderr)
                usage = subprocess.run([str(app / "linqu_mem_service_provider_obmm")],
                                       capture_output=True, text=True, timeout=10)
                self.assertEqual(usage.returncode, 2)
                self.assertEqual("serve-allocations" in usage.stderr, enabled)
                self.assertEqual("resume-allocations" in usage.stderr, enabled)
                self.assertEqual("quarantine-allocation-state" in usage.stderr, enabled)
                self.assertEqual("prepare-holder-rejoin" in usage.stderr, enabled)

    def test_worker_build_and_startup_guards(self):
        native = platform.system() == "Linux" and platform.machine() == "aarch64"
        compiler = shutil.which("cc" if native else "aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("requires AArch64 compiler")
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "obmm-worker"
            sources = [
                PROVIDERS / "mem_service_provider_obmm_cli.c",
                PROVIDERS / "mem_service_provider_obmm_worker.c",
                PROVIDERS / "mem_service_provider_obmm.c",
                COMPONENT / "mem_service_provider.c",
                COMPONENT / "mem_service_client.c",
                COMPONENT / "mem_service_wire_client.c",
                MANAGER / "gva_manager.c",
                SERVICE / "vendor/obmm/src/libobmm/libobmm.c",
                SERVICE / "common/obmm_vendor_adaptor_sim.c",
            ]
            command = [compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-DMEM_SERVICE_OBMM_MANAGED_WORKER", "-D__EXPORTED_HEADERS__"]
            for include in [SERVICE, MANAGER, SERVICE / "vendor/obmm/src/libobmm",
                            SERVICE / "kernel_ub/include/uapi", SERVICE / "kernel_ub/include"]:
                command.extend(["-I", str(include)])
            built = subprocess.run(command + list(map(str, sources)) + ["-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            golden = pathlib.Path(directory) / "worker-golden"
            wrapped = ["gva_manager_allocate_segment", "gva_manager_export_segment_checked",
                       "mem_service_client_publish_allocation", "gva_manager_unexport_segment",
                       "gva_manager_retire_segment", "mem_service_client_reclaim_allocation",
                       "mem_service_client_retire_object",
                       "open", "close", "ioctl", "fsync", "mem_service_client_provider_refresh",
                       "mem_service_client_poll_allocation", "mem_service_client_provider_deregister",
                       "mem_service_client_poll_recovery_allocation",
                       "mem_service_client_poll_holder_recovery_allocation",
                       "mem_service_client_fence_allocation_holder",
                       "mem_service_client_recover_allocation",
                       "mem_service_client_inspect_allocation"]
            built = subprocess.run(
                command + [str(ROOT / "guest-linux/aarch64/tests/mem_service_gva_worker_golden.c")]
                + list(map(str, sources[2:]))
                + [f"-Wl,--wrap={name}" for name in wrapped] + ["-o", str(golden)],
                capture_output=True, text=True,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            if not native:
                return
            result = subprocess.run([str(golden)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("gva_worker_transaction=pass", result.stdout)
            self.assertIn("gva_worker_kernel_reconcile=pass cases=8 descriptor_bytes=72 startup_guard=1",
                          result.stdout)
            self.assertIn("gva_worker_durable_ledger=pass identity=1 corruption=1 torn=1 writer_lock=1",
                          result.stdout)
            self.assertIn(
                "gva_worker_recovery=pass kernel_replaced=1 "
                "home_holder_fenced=1 service_reclaim=1 archive_retry=1",
                result.stdout,
            )
            self.assertIn(
                "gva_worker_holder_fence=pass physical_first=1 "
                "failed_physical_receipt=0 exact_identity=1",
                result.stdout,
            )
            self.assertIn(
                "gva_worker_holder_rejoin=pass nonempty=reject "
                "home_pending=reject holder_pending=reject archive=1 retry=1",
                result.stdout,
            )
            self.assertIn(
                "gva_worker_same_kernel_resume=pass reservations=1 "
                "kernel_mismatch=reject service_mismatch=reject",
                result.stdout,
            )
            self.assertIn(
                "gva_worker_partial_resume=pass stages=9 "
                "reserve_intent=isolated torn_frame=isolated",
                result.stdout,
            )
            self.assertIn(
                "gva_worker_quarantine=pass complete=1 invalid_tail=1 torn_tail=1 "
                "invalid_start=reject legacy_start=reject writer_lock=reject "
                "idempotent_states=3",
                result.stdout,
            )
            self.assertIn(
                "gva_worker_fault_injection=pass reserve_intent=durable "
                "torn_frame=durable quarantine=2",
                result.stdout,
            )
            config = pathlib.Path(directory) / "worker.conf"
            state = pathlib.Path(directory) / "worker.state"
            good = (f"connect=unix:{directory}/absent.sock\nnode_id=nodeA\n"
                    f"state_file={state}\nincarnation=1\nreadiness_generation=1\n"
                    "allocation_granularity_bytes=2097152\n")
            for content in ["", good + "unknown=1\n", good + "node_id=duplicate\n",
                            good + "fast_allocation=2\n", good + "fast_allocation=-1\n",
                            good + "fast_allocation=0\nfast_allocation=1\n",
                            good + "fault_injection_phase=reserve-intent\n",
                            good + "fault_injection_mode=sync-exit\n",
                            good + "fault_injection_phase=reserved\n"
                                   "fault_injection_mode=sync-exit\n",
                            good + "fault_injection_phase=reserve-intent\n"
                                   "fault_injection_mode=invalid\n",
                            good.replace("incarnation=1", "incarnation=-1"),
                            good.replace("allocation_granularity_bytes=2097152\n", ""),
                            good.replace("allocation_granularity_bytes=2097152", "allocation_granularity_bytes=3")]:
                config.write_text(content)
                result = subprocess.run([str(binary), "serve-allocations", "--config", str(config)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse(state.exists())
            state.write_text("unreconciled reservation\n")
            for optional in ("", "fast_allocation=0\n", "fast_allocation=1\n"):
                config.write_text(good + optional)
                result = subprocess.run([str(binary), "serve-allocations", "--config", str(config)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("reconciliation required", result.stderr)
                self.assertEqual(state.read_text(), "unreconciled reservation\n")
                inspected = subprocess.run([str(binary), "inspect-allocation-state", "--config", str(config)],
                                           capture_output=True, text=True, timeout=10)
                self.assertEqual(inspected.returncode, 1, inspected.stderr)
                self.assertIn("physical_state=unknown", inspected.stderr)
                self.assertEqual(state.read_text(), "unreconciled reservation\n")


if __name__ == "__main__":
    unittest.main()
