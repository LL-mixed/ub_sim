import os
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

    def test_bootstrap_wrapper_uses_lingqu_memory_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            weights = tmp / "weights"
            weights.mkdir()
            env_file = tmp / "w5-memory.env"
            fake_sim_cli = tmp / "sim-cli"
            fake_sim_cli.write_text(
                "\n".join(
                    [
                        "#!/bin/sh",
                        "test \"$1\" = lingqu-memory || exit 21",
                        "test \"$2\" = bootstrap-w5-service || exit 22",
                        "printf \"%s\\n\" \"export SIM_W5_MEMORY_SERVICE='lingqu_memory_service'\"",
                        "printf \"%s\\n\" \"export SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED='1'\"",
                        "printf \"%s\\n\" \"export SIM_W5_MEMORY_STORE='/tmp/w5-memory.json'\"",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            fake_sim_cli.chmod(0o755)
            env = {
                **os.environ,
                "SIM_CLI_BIN": str(fake_sim_cli),
                "SIM_QWEN3_DENSE_WEIGHTS_PATH": str(weights),
                "RUN_ID": "w5_bootstrap_test",
                "SIM_UAPI_W5_PROFILE": "qwen3_0_6b_decode",
            }

            result = subprocess.run(
                [str(self.bootstrap), "--print-env", "--env-file", str(env_file)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )

            self.assertEqual(env_file.read_text(encoding="utf-8"), result.stdout)

        self.assertIn("SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED='1'", result.stdout)
        self.assertIn("SIM_W5_MEMORY_STORE='/tmp/w5-memory.json'", result.stdout)

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
                        "if test \"$1\" = lingqu-memory && test \"$2\" = bootstrap-w5-service; then",
                        "  printf \"%s\\n\" \"export SIM_W5_MEMORY_SERVICE='lingqu_memory_service'\"",
                        "  printf \"%s\\n\" \"export SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED='1'\"",
                        "  printf \"%s\\n\" \"export SIM_W5_MEMORY_STORE='$SIM_W5_MEMORY_STORE'\"",
                        "  printf \"%s\\n\" \"export SIM_W5_MEMORY_OBJECT_STORE='$SIM_W5_MEMORY_OBJECT_STORE'\"",
                        "  printf \"%s\\n\" \"export SIM_W5_MEMORY_ENGRAM_STATE='$SIM_W5_MEMORY_ENGRAM_STATE'\"",
                        "  printf \"%s\\n\" \"export SIM_W5_MEMORY_REGISTRY_DIR='$SIM_W5_MEMORY_REGISTRY_DIR'\"",
                        "  exit 0",
                        "fi",
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

    def test_bootstrap_boundary_stays_out_of_runtime_script(self) -> None:
        runtime_text = self.runtime.read_text(encoding="utf-8")
        config_text = self.config_runner.read_text(encoding="utf-8")

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


if __name__ == "__main__":
    unittest.main()
