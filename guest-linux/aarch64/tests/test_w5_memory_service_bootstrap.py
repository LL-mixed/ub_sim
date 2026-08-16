import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class W5MemoryServiceBootstrapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Path(__file__).resolve().parents[3]
        self.scripts = self.repo / "guest-linux" / "aarch64" / "scripts"
        self.runtime = self.scripts / "run_w5_inference_cluster_runtime.sh"
        self.config_runner = self.scripts / "run_w5_cluster_config.sh"
        self.bootstrap = self.scripts / "run_w5_memory_service_bootstrap.sh"
        self.guest_runner = self.scripts / "run_llm_infer_eight_node_guest.sh"
        self.headless_launcher = self.scripts / "launch_ub_eight_node_headless.sh"
        self.mem_service_app = (
            Path(
                os.environ.get("MEM_SERVICE_ROOT", self.repo / "mem_service")
            )
            / "apps"
            / "mem_service"
        )

    def test_runtime_refuses_memory_path_without_infra_bootstrap(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            fake_sim_cli = tmp / "sim-cli"
            fake_sim_cli.write_text("#!/bin/sh\nexit 99\n", encoding="utf-8")
            fake_sim_cli.chmod(0o755)
            env = {
                **os.environ,
                "SIM_CLI_BIN": str(fake_sim_cli),
                "SIM_QWEN3_DENSE_WEIGHTS_PATH": str(tmp / "weights"),
                "SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP": "1",
                "SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED": "0",
                "SIM_W5_SERVING_QUEUE": "1",
                "SIM_W5_TEST_VALIDATE_ONLY": "1",
            }

            result = subprocess.run(
                [str(self.runtime)],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "W5 Memory Service runtime path requires infrastructure bootstrap before infer launch",
            result.stderr,
        )
        self.assertNotIn("serving_queue=1 launch_mode=ready_only", result.stderr)

    def test_bootstrap_wrapper_uses_mem_service_host_binary(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            env_file = tmp / "w5-memory.env"
            fake_sim_cli = tmp / "sim-cli"
            fake_sim_cli.write_text("#!/bin/sh\nexit 99\n", encoding="utf-8")
            fake_sim_cli.chmod(0o755)
            env = {
                **os.environ,
                "SIM_CLI_BIN": str(fake_sim_cli),
                "RUN_ID": "w5_bootstrap_test",
                "SIM_UAPI_W5_PROFILE": "qwen3_0_6b_decode",
                "SIM_W5_MEMORY_STORE": str(tmp / "memory-store.json"),
                "SIM_W5_MEMORY_OBJECT_STORE": str(tmp / "object-store.json"),
                "SIM_W5_MEMORY_ENGRAM_STATE": str(tmp / "engram-state.json"),
                "SIM_W5_MEMORY_REGISTRY_DIR": str(tmp / "registry"),
            }

            result = subprocess.run(
                [str(self.bootstrap), "--print-env", "--env-file", str(env_file)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )

            self.assertEqual(env_file.read_text(encoding="utf-8"), result.stdout)
            self.assertTrue((tmp / "object-store.json").is_file())
            self.assertTrue((tmp / "registry").is_dir())

        self.assertIn("SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED='1'", result.stdout)
        self.assertIn("SIM_W5_MEMORY_STORE='", result.stdout)
        self.assertNotIn("sim-cli", result.stderr)

    def test_cluster_config_bootstraps_before_runtime_validate_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            weights = tmp / "weights"
            weights.mkdir()
            env_file = tmp / "w5.env"
            bootstrap_env = tmp / "w5-memory.env"
            fake_sim_cli = tmp / "sim-cli"
            fake_sim_cli.write_text(
                "\n".join(
                    [
                        "#!/bin/sh",
                        "exit 23",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            fake_sim_cli.chmod(0o755)
            env_file.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode",
                        f"SIM_QWEN3_DENSE_WEIGHTS_PATH={weights}",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        f"SIM_CLI_BIN={fake_sim_cli}",
                        f"SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE={bootstrap_env}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    str(self.config_runner),
                    "--validate-only",
                    "--serve-queue",
                    str(env_file),
                ],
                capture_output=True,
                text=True,
                check=True,
            )

            self.assertTrue(bootstrap_env.exists())
        self.assertIn("config validation passed: Memory Service bootstrap ready", result.stderr)

    def test_flash_readiness_only_skips_qwen3_weights_and_bootstrap(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            env_file = tmp / "w5-flash.env"
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            cargo_log = tmp / "cargo.log"
            fake_cargo = fake_bin / "cargo"
            fake_cargo.write_text(
                "\n".join(
                    [
                        "#!/bin/sh",
                        f"printf '%s\\n' \"$*\" >> {cargo_log}",
                        "exit 0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            fake_cargo.chmod(0o755)
            fake_cc = shutil.which("true") or "/bin/true"
            env_file.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode",
                        "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_GUEST_ENGRAM=0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            env = {
                **os.environ,
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
                "AARCH64_LINUX_CC": fake_cc,
            }

            result = subprocess.run(
                [str(self.config_runner), "--readiness-only", str(env_file)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )

            cargo_calls = cargo_log.read_text(encoding="utf-8")

        self.assertIn("profile=deepseek_v4_flash_decode readiness_only=1", result.stdout)
        self.assertIn("[build_initramfs] W5 guest link check passed", result.stdout)
        self.assertIn("qwen3-decode-loop", cargo_calls)
        self.assertIn("--profile=deepseek-v4-flash", cargo_calls)
        self.assertIn("deepseek-v4-flash-weight-catalog", cargo_calls)
        self.assertIn("--source-kind fixture", cargo_calls)
        self.assertIn("--weight-catalog", cargo_calls)
        self.assertIn("deepseek-v4-flash-moe-report", cargo_calls)
        self.assertIn("--route-trace", cargo_calls)
        self.assertIn(
            "crates/sim-models/fixtures/deepseek_v4_flash_route_trace.ds4.txt",
            cargo_calls,
        )
        self.assertIn("--route-trace-manifest", cargo_calls)
        self.assertIn(
            "crates/sim-models/fixtures/deepseek_v4_flash_route_trace.manifest.txt",
            cargo_calls,
        )
        self.assertIn("--require-route-source-kind fixture", cargo_calls)
        self.assertIn("--weight-provider", cargo_calls)
        self.assertIn(
            "crates/sim-models/fixtures/deepseek_v4_flash_weight_provider.fixture.txt",
            cargo_calls,
        )
        self.assertIn(
            "crates/sim-models/fixtures/deepseek_v4_flash_weight_provider.file.fixture.txt",
            cargo_calls,
        )
        self.assertNotIn("W5 cluster config requires SIM_QWEN3_DENSE_WEIGHTS_PATH", result.stderr)
        self.assertNotIn("W5 Memory Service bootstrap", result.stderr)

    def test_flash_readiness_uses_measured_trace_gate_when_manifest_is_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            env_file = tmp / "w5-flash.env"
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            cargo_log = tmp / "cargo.log"
            fake_cargo = fake_bin / "cargo"
            fake_cargo.write_text(
                "\n".join(
                    [
                        "#!/bin/sh",
                        f"printf '%s\\n' \"$*\" >> {cargo_log}",
                        "exit 0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            fake_cargo.chmod(0o755)
            fake_cc = shutil.which("true") or "/bin/true"
            env_file.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode",
                        "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_GUEST_ENGRAM=0",
                        "SIM_W5_TEST_FLASH_ROUTE_TRACE_MANIFEST=/tmp/ds4.route.manifest",
                        "SIM_W5_TEST_FLASH_WEIGHT_PROVIDER=/tmp/ds4.weight.provider",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            env = {
                **os.environ,
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
                "AARCH64_LINUX_CC": fake_cc,
            }

            subprocess.run(
                [str(self.config_runner), "--readiness-only", str(env_file)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )

            cargo_calls = cargo_log.read_text(encoding="utf-8")

        self.assertIn("--route-trace-manifest /tmp/ds4.route.manifest", cargo_calls)
        self.assertIn("--require-route-source-kind ds4-measured", cargo_calls)
        self.assertIn("deepseek-v4-flash-weight-catalog", cargo_calls)
        self.assertIn("--from-provider /tmp/ds4.weight.provider", cargo_calls)
        self.assertIn("--source-kind ds4-measured", cargo_calls)
        self.assertIn("--weight-catalog", cargo_calls)

    def test_flash_readiness_builds_catalog_from_payload_dir_option(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            env_file = tmp / "w5-flash.env"
            payload_dir = tmp / "payloads"
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            payload_dir.mkdir()
            cargo_log = tmp / "cargo.log"
            fake_cargo = fake_bin / "cargo"
            fake_cargo.write_text(
                "\n".join(
                    [
                        "#!/bin/sh",
                        f"printf '%s\\n' \"$*\" >> {cargo_log}",
                        "exit 0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            fake_cargo.chmod(0o755)
            fake_cc = shutil.which("true") or "/bin/true"
            env_file.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode",
                        "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_GUEST_ENGRAM=0",
                        "SIM_W5_TEST_FLASH_ROUTE_TRACE_MANIFEST=/tmp/ds4.route.manifest",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            env = {
                **os.environ,
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
                "AARCH64_LINUX_CC": fake_cc,
            }

            subprocess.run(
                [
                    str(self.config_runner),
                    "--readiness-only",
                    "--flash-payload-dir",
                    str(payload_dir),
                    str(env_file),
                ],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )

            cargo_calls = cargo_log.read_text(encoding="utf-8")

        self.assertIn("deepseek-v4-flash-weight-catalog", cargo_calls)
        self.assertIn(f"--payload-dir {payload_dir}", cargo_calls)
        self.assertIn("--source-kind ds4-measured", cargo_calls)
        self.assertIn("--route-trace-manifest /tmp/ds4.route.manifest", cargo_calls)
        self.assertIn("--require-route-source-kind ds4-measured", cargo_calls)
        self.assertIn("--weight-catalog", cargo_calls)
        self.assertNotIn("--from-provider", cargo_calls)

    def test_flash_readiness_uses_weight_catalog_when_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            env_file = tmp / "w5-flash.env"
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            cargo_log = tmp / "cargo.log"
            fake_cargo = fake_bin / "cargo"
            fake_cargo.write_text(
                "\n".join(
                    [
                        "#!/bin/sh",
                        f"printf '%s\\n' \"$*\" >> {cargo_log}",
                        "exit 0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            fake_cargo.chmod(0o755)
            fake_cc = shutil.which("true") or "/bin/true"
            env_file.write_text(
                "\n".join(
                    [
                        "SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode",
                        "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash",
                        "SIM_QWEN3_GUEST_DECODE_STEPS=2",
                        "SIM_QWEN3_GUEST_ENGRAM=0",
                        "SIM_W5_TEST_FLASH_ROUTE_TRACE_MANIFEST=/tmp/ds4.route.manifest",
                        "SIM_W5_TEST_FLASH_WEIGHT_CATALOG=/tmp/ds4.weight.catalog",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            env = {
                **os.environ,
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
                "AARCH64_LINUX_CC": fake_cc,
            }

            subprocess.run(
                [str(self.config_runner), "--readiness-only", str(env_file)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )

            cargo_calls = cargo_log.read_text(encoding="utf-8")

        self.assertIn("--route-trace-manifest /tmp/ds4.route.manifest", cargo_calls)
        self.assertIn("--require-route-source-kind ds4-measured", cargo_calls)
        self.assertIn("--weight-catalog /tmp/ds4.weight.catalog", cargo_calls)
        self.assertNotIn("deepseek-v4-flash-weight-catalog", cargo_calls)
        self.assertNotIn("--weight-provider /tmp/ds4.weight.provider", cargo_calls)

    def test_flash_runtime_defaults_skip_qwen3_memory_reuse(self) -> None:
        runtime_text = self.runtime.read_text(encoding="utf-8")
        self.assertIn(
            'deepseek_v4_flash_decode)\n'
            '    SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP="${SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-0}"',
            runtime_text,
        )
        self.assertIn(
            'deepseek_v4_flash_decode)\n'
            '    SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP:-0}"',
            runtime_text,
        )

    def test_flash_weight_catalog_is_runtime_config_and_staged_for_guest(self) -> None:
        config_text = self.config_runner.read_text(encoding="utf-8")
        runtime_text = self.runtime.read_text(encoding="utf-8")
        guest_text = self.guest_runner.read_text(encoding="utf-8")
        headless_text = self.headless_launcher.read_text(encoding="utf-8")

        self.assertIn("SIM_W5_FLASH_WEIGHT_CATALOG", config_text)
        self.assertIn(
            '${SIM_W5_FLASH_WEIGHT_CATALOG:-${SIM_W5_TEST_FLASH_WEIGHT_CATALOG:-}}',
            runtime_text,
        )
        self.assertIn("export SIM_W5_FLASH_WEIGHT_CATALOG", runtime_text)
        self.assertIn("stage_flash_weight_catalog", guest_text)
        self.assertIn(
            'local catalog_guest_path="/tmp/deepseek_v4_flash_weight.catalog"',
            guest_text,
        )
        self.assertIn(
            'export SIM_W5_FLASH_WEIGHT_CATALOG="$SIM_W5_FLASH_WEIGHT_CATALOG_GUEST"',
            guest_text,
        )
        self.assertIn("SIM_W5_FLASH_WEIGHT_CATALOG_GUEST", guest_text)
        self.assertIn("SIM_W5_FLASH_WEIGHT_CATALOG", headless_text)
        self.assertNotIn(
            'export SIM_W5_FLASH_WEIGHT_CATALOG="$SIM_W5_FLASH_WEIGHT_CATALOG"',
            guest_text,
        )

    def test_bootstrap_boundary_stays_out_of_runtime_script(self) -> None:
        runtime_text = self.runtime.read_text(encoding="utf-8")
        config_text = self.config_runner.read_text(encoding="utf-8")
        bootstrap_text = self.bootstrap.read_text(encoding="utf-8")
        sim_cli_text = (self.repo / "crates" / "sim-cli" / "src" / "main.rs").read_text(
            encoding="utf-8"
        )
        mem_service_text = (
            self.mem_service_app / "mem_service.c"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "W5 Memory Service runtime path requires infrastructure bootstrap before infer launch",
            runtime_text,
        )
        self.assertNotIn(
            '"$SCRIPT_DIR/run_w5_memory_service_bootstrap.sh"',
            runtime_text,
        )
        self.assertIn("bootstrap_w5_memory_service_infra", config_text)
        self.assertIn(
            '"$SCRIPT_DIR/run_w5_memory_service_bootstrap.sh"',
            config_text,
        )
        self.assertIn("linqu_mem_service_host", bootstrap_text)
        self.assertIn('"$MEM_SERVICE_HOST_BIN" bootstrap-w5-service', bootstrap_text)
        self.assertIn('make -C "$MEM_SERVICE_APP_DIR" linqu_mem_service_host >&2', bootstrap_text)
        self.assertNotIn("SIM_CLI_BIN", bootstrap_text)
        self.assertNotIn("cargo build -p sim-cli", bootstrap_text)
        self.assertNotIn("lingqu-memory bootstrap-w5-service", bootstrap_text)
        self.assertNotIn('"bootstrap-w5-service" =>', sim_cli_text)
        self.assertNotIn("run_lingqu_memory_bootstrap_w5_service_cli", sim_cli_text)
        self.assertIn("run_bootstrap_w5_service", mem_service_text)


if __name__ == "__main__":
    unittest.main()
