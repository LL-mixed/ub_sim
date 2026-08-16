from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = (
    REPO_ROOT
    / "guest-linux"
    / "aarch64"
    / "scripts"
    / "dgx_ds4_mtp_canary.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("dgx_ds4_mtp_canary", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class DgxDs4MtpCanaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.canary = load_module()

    def args(self, **overrides):
        values = {
            "prompt_file": Path("/tmp/prompt.txt"),
            "ssh_host": "dgx1",
            "endpoint": "http://192.168.8.7:8000",
            "benchmark_via_ssh": False,
            "remote_dir": "/home/dgx/repo/ds4",
            "q2_model": self.canary.DEFAULT_Q2_MODEL,
            "mtp_model": self.canary.DEFAULT_MTP_MODEL,
            "mtp_mode": "instrumented-fast",
            "mtp_margin": 3.0,
            "ctx": 4096,
            "max_tokens": 1024,
            "runs": 3,
            "warmup_runs": 1,
            "request_timeout": 600,
            "startup_timeout": 180,
            "output_dir": None,
            "dry_run": False,
        }
        values.update(overrides)
        return argparse.Namespace(**values)

    def test_server_commands_are_single_node_and_never_enable_ssd_streaming(self):
        baseline = self.canary.server_argv(self.args(), mtp=False)
        mtp = self.canary.server_argv(self.args(), mtp=True)

        self.assertNotIn("--role", baseline)
        self.assertNotIn("--ssd-streaming", baseline)
        self.assertNotIn("--mtp", baseline)
        self.assertEqual(mtp[mtp.index("--mtp-draft") + 1], "2")
        self.assertEqual(mtp[mtp.index("--mtp-margin") + 1], "3")

    def test_mtp_modes_select_only_the_requested_environment(self):
        instrumented = self.canary.mtp_environment(self.args())
        strict = self.canary.mtp_environment(
            self.args(mtp_mode="clean-strict")
        )
        replay = self.canary.mtp_environment(
            self.args(mtp_mode="clean-exact-replay")
        )

        self.assertIn("DS4_MTP_TIMING", instrumented)
        self.assertEqual(strict, {"DS4_MTP_STRICT": "1"})
        self.assertEqual(replay, {"DS4_MTP_EXACT_REPLAY": "1"})
        self.assertEqual(
            self.canary.mtp_environment(self.args(mtp_mode="clean-fast")), {}
        )

    def test_benchmark_command_keeps_common_prompt_and_sampling_shape(self):
        command = self.canary.benchmark_argv(
            self.args(), Path("/tmp/result.json"), "q2-mtp-draft2"
        )

        self.assertEqual(command[0], self.canary.sys.executable)
        self.assertEqual(command[command.index("--runs") + 1], "3")
        self.assertEqual(command[command.index("--warmup-runs") + 1], "1")
        self.assertEqual(command[command.index("--max-tokens") + 1], "1024")
        self.assertEqual(command[command.index("--label") + 1], "q2-mtp-draft2")

    def test_remote_benchmark_command_uses_loopback_and_remote_files(self):
        args = self.args(benchmark_via_ssh=True)
        runner = self.canary.CanaryRunner(args)
        command = self.canary.benchmark_argv(
            args,
            "/tmp/result.json",
            "clean-strict",
            python="python3",
            fetch_script="/tmp/fetch.py",
            endpoint=runner.benchmark_endpoint(),
            prompt_file="/tmp/prompt.txt",
        )

        self.assertEqual(command[0], "python3")
        self.assertEqual(command[command.index("--endpoint") + 1], "http://127.0.0.1:8000")
        self.assertEqual(command[command.index("--prompt-file") + 1], "/tmp/prompt.txt")

    def test_comparison_requires_matching_output_and_exercised_drafts(self):
        def report(tpot):
            return {
                "summary": {"tpot_ms_median": tpot},
                "runs": [{"output_text": "same"}, {"output_text": "same"}],
            }

        comparison = self.canary.build_comparison(
            report(100.0),
            report(80.0),
            (
                "ds4: mtp timing micro drafted=2 committed=2 draft=1 ms\n"
                "ds4: mtp timing micro drafted=2 committed=1 draft=1 ms\n"
            ),
        )

        self.assertEqual(comparison["status"], "pass")
        self.assertTrue(comparison["correctness"]["outputs_match"])
        self.assertEqual(comparison["speculation"]["multi_token_drafts"], 2)
        self.assertEqual(comparison["speculation"]["multi_token_commits"], 1)
        self.assertEqual(comparison["performance"]["speedup"], 1.25)

    def test_comparison_fails_when_greedy_outputs_diverge(self):
        baseline = {
            "summary": {"tpot_ms_median": 100.0},
            "runs": [{"output_text": "baseline"}],
        }
        mtp = {
            "summary": {"tpot_ms_median": 90.0},
            "runs": [{"output_text": "different"}],
        }

        comparison = self.canary.build_comparison(
            baseline, mtp, "mtp timing micro drafted=2 committed=2"
        )

        self.assertEqual(comparison["status"], "fail")
        self.assertFalse(comparison["correctness"]["outputs_match"])

    def test_clean_mode_uses_model_load_as_non_timing_evidence(self):
        report = {
            "summary": {"tpot_ms_median": 100.0},
            "runs": [{"output_text": "same"}],
        }

        comparison = self.canary.build_comparison(
            report,
            report,
            "ds4: MTP support model loaded: mtp.gguf (draft=2)",
            require_timing_evidence=False,
        )

        self.assertEqual(comparison["status"], "pass")
        self.assertTrue(comparison["speculation"]["model_loaded"])

    def test_dry_run_does_not_require_prompt_or_remote_access(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.args(
                prompt_file=Path(directory) / "missing.txt",
                output_dir=Path(directory) / "artifacts",
                dry_run=True,
            )
            runner = self.canary.CanaryRunner(args)

            result = runner.run()

            self.assertEqual(result, args.output_dir.resolve())
            self.assertFalse(args.output_dir.exists())

    def test_log_copy_failure_does_not_skip_coordinator_restore(self):
        canary = self.canary

        class FakeRunner(canary.CanaryRunner):
            def __init__(self, args):
                super().__init__(args)
                self.restore_called = False

            def preflight(self):
                self.original = canary.RemoteService(
                    pid=10,
                    cwd=self.args.remote_dir,
                    argv=[
                        "./ds4-server",
                        "--role",
                        "coordinator",
                        "--layers",
                        "0:14",
                        "--listen",
                        "192.168.8.7",
                        "12340",
                    ],
                    stdout_path="/tmp/original.log",
                )

            def write_remote_env(self):
                return None

            def stop_process(self, pid):
                return None

            def wait_http(self, expected_up):
                return None

            def start_service(self, argv, cwd, log_path, **kwargs):
                return 20

            def run_benchmark(self, label, output_path):
                output_path.write_text(
                    json.dumps(
                        {
                            "summary": {"tpot_ms_median": 100.0},
                            "runs": [{"output_text": "same"}],
                        }
                    ),
                    encoding="utf-8",
                )

            def copy_remote_log(self, remote_path_value, local_path):
                raise canary.CanaryError("synthetic copy failure")

            def restore_original(self):
                self.restore_called = True
                self.original_stopped = False

        with tempfile.TemporaryDirectory() as directory:
            prompt = Path(directory) / "prompt.txt"
            prompt.write_text("hello", encoding="utf-8")
            output_dir = Path(directory) / "artifacts"
            runner = FakeRunner(
                self.args(prompt_file=prompt, output_dir=output_dir)
            )

            with self.assertRaisesRegex(
                canary.CanaryError, "synthetic copy failure"
            ):
                runner.run()

            self.assertTrue(runner.restore_called)
            state = json.loads(
                (output_dir / "canary-state.json").read_text(encoding="utf-8")
            )
            self.assertTrue(state["coordinator_restored"])
            self.assertIn("cleanup_errors", state)
            event_log = (output_dir / "canary.log").read_text(encoding="utf-8")
            self.assertIn("stage=restore-coordinator", event_log)
            self.assertIn("cleanup-error=copy", event_log)


if __name__ == "__main__":
    unittest.main()
