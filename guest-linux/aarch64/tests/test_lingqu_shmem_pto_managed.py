import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
APP = ROOT / "guest-linux/aarch64/apps/lingqu_shmem_pto_direct"
NATIVE = platform.system() == "Linux" and platform.machine() == "aarch64"
CONFIG = {
    "connect": "unix:/tmp/managed-pto-validation-must-not-connect.sock",
    "key": "managed-pto-test", "node_id": "1", "generation": "135",
    "requester_cna": "0xf002", "artifact_fingerprint": "0x1234",
    "timeout_ms": "1000",
}


@unittest.skipUnless(NATIVE and shutil.which("cc"), "native Linux AArch64 CLI fixture")
class ManagedPtoCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "managed-pto"
        result = subprocess.run([
            "make", "-C", str(APP), "CC=cc", "MANAGED=1", f"TARGET={cls.binary}",
        ], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_config(self, values=None, extra="", raw=None):
        config = pathlib.Path(self.directory.name) / "input.conf"
        if raw is None:
            raw = "".join(f"{key}={value}\n" for key, value in
                          (CONFIG if values is None else values).items()) + extra
        config.write_text(raw)
        return subprocess.run([str(self.binary), "--managed-config", str(config),
                               "--validate-only"], capture_output=True, text=True, timeout=5)

    def test_validate_only_accepts_both_roles_without_device_or_network(self):
        for node in ("0", "1"):
            with self.subTest(node=node):
                result = self.run_config({**CONFIG, "node_id": node})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"status=ok node={node} nodes=2", result.stdout)
                self.assertIn("network_operations=0", result.stdout)
                self.assertNotIn("peer_canary", result.stdout)
        result = self.run_config({**CONFIG, "connect": "tcp:127.0.0.1:9700"})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_missing_duplicate_unknown_and_malformed_fields(self):
        for key in CONFIG:
            values = dict(CONFIG)
            values.pop(key)
            with self.subTest(missing=key):
                self.assertEqual(self.run_config(values).returncode, 2)
            with self.subTest(duplicate=key):
                self.assertEqual(self.run_config(extra=f"{key}={CONFIG[key]}\n").returncode, 2)
        bad = {"node_id": ("2", "-1"), "generation": ("0", "18446744073709551615"),
               "requester_cna": ("0", "4294967296"), "artifact_fingerprint": ("0", "bad"),
               "timeout_ms": ("0", "600001", " 50"), "key": ("", "x" * 100, "a b"),
               "connect": ("tcp:", "tcp:127.0.0.1:0", "tcp:127.0.0.1:65536",
                           "tcp:bad:80", "unix:", "unix:relative", "http://example.test")}
        for key, values in bad.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.assertEqual(self.run_config({**CONFIG, key: value}).returncode, 2)
        for extra in ("unknown=1\n", "missing_equals\n", "key=hidden\0tail\n", "\rhidden\n"):
            with self.subTest(extra=extra):
                self.assertEqual(self.run_config(extra=extra).returncode, 2)

    def test_managed_binary_has_no_model_arena_dependency(self):
        symbols = subprocess.check_output(["nm", str(self.binary)], text=True)
        self.assertIn("lingqu_shmem_mem_service_open_managed", symbols)
        self.assertIn("mem_service_provider_obmm_mapping_pin_acquire", symbols)
        self.assertNotIn("mem_service_cluster_runtime_current", symbols)
        self.assertNotIn("mem_service_model_kv_state_alloc", symbols)

    def test_optional_submit_timeout_requires_bounded_recovery(self):
        result = self.run_config({**CONFIG, "submit_timeout_ms": "5"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("submit_timeout_ms=5 recovery_expected=1", result.stdout)
        for value in ("0", "1000", "1001", "600001", "-1", "5x"):
            with self.subTest(value=value):
                self.assertEqual(self.run_config({**CONFIG, "submit_timeout_ms": value}).returncode, 2)
        self.assertEqual(self.run_config(extra="submit_timeout_ms=5\nsubmit_timeout_ms=6\n").returncode, 2)


class ManagedPtoContractTest(unittest.TestCase):
    def test_same_mapping_two_dispatches_and_fail_closed_lifetime(self):
        source = (APP / "lingqu_shmem_pto_managed.c").read_text()
        self.assertIn("mem_service_client_map_managed_allocation", source)
        self.assertIn("lingqu_shmem_mem_service_acquire_managed", source)
        self.assertIn("unsigned vectors[3] = {round == 1 ? 0 : 2, 1, round + 1}", source)
        self.assertIn("dispatch_vector(state, 1)", source)
        self.assertIn("dispatch_vector(state, 2)", source)
        self.assertNotIn("mem_service_client_put_object", source)
        self.assertNotIn("obmm_do_import", source)
        self.assertNotIn("obmm_async_map_register", source)
        self.assertIn("state->completion_unknown = true", source)
        self.assertIn("if (state->completion_unknown) return -EBUSY", source)
        self.assertIn("resources_retained=1", source)
        self.assertIn("for (;;) pause()", source)
        self.assertIn("lingqu_shmem_pto_endpoint_wait(state->endpoint, request.op_id", source)
        self.assertIn("lingqu_shmem_pto_endpoint_close_checked(state->endpoint) != -EBUSY", source)
        self.assertIn("state->config.recovery_expected && !recovered", source)

    def test_initramfs_and_dual_node_runner_expose_managed_mode(self):
        build = (ROOT / "guest-linux/aarch64/scripts/build_initramfs.sh").read_text()
        run_app = (ROOT / "guest-linux/aarch64/initramfs/run_app").read_text()
        runner = (ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh").read_text()
        self.assertIn("-DLINGQU_SHMEM_MANAGED_MAPPING", build)
        self.assertIn('"$LINGQU_SHMEM_PTO_MANAGED_SRC"', build)
        self.assertIn('"$MEM_SERVICE_PROVIDER_SRC" "$MEM_SERVICE_PROVIDER_OBMM_SRC"', build)
        self.assertIn("--managed-config /tmp/managed-share.pto.conf", run_app)
        self.assertIn('access="${8:-cpu}"', run_app)
        self.assertIn("mem_service_object_share_access cpu", run_app)
        self.assertIn("managed_pto_selected()", runner)
        self.assertIn('"${SCRIPT_DIR}/validate_managed_pto.py"', runner)
        self.assertIn("MANAGED_PTO_RESULT .*status=fail", runner)
        self.assertIn("lingqu_shmem_pto_submit_timeout_ms", run_app)
        self.assertIn("--require-timeout-recovery", runner)

    @unittest.skipUnless(shutil.which("zsh"), "zsh runner required")
    def test_runner_rejects_unsupported_access_before_guest_launch(self):
        runner = ROOT / "guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh"
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / "runner.env"
            for access, expected in (("unknown", "unsupported Memory Service access mode"),
                                     ("cpu-pto", "cpu-pto requires OBMM")):
                with self.subTest(access=access):
                    config.write_text(f"MEM_SERVICE_OBJECT_SHARE_ACCESS={access}\n"
                                      "MEM_SERVICE_OBJECT_SHARE_BACKEND=loopback\n")
                    result = subprocess.run(["zsh", str(runner), "--config", str(config),
                                             "--app", "mem_service_object_share"],
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn(expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
