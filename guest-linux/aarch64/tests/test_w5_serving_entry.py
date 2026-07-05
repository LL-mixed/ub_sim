#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class W5ServingEntryTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path(__file__).resolve().parents[3]
        self.script_dir = self.repo / "guest-linux" / "aarch64" / "scripts"
        self.entry_py = self.script_dir / "w5_serving_entry.py"
        self.entry_sh = self.script_dir / "run_w5_serving_entry.sh"
        self.submit_py = self.script_dir / "w5_serving_submit.py"

    def run_entry(self, *args):
        return subprocess.run(
            [sys.executable, str(self.entry_py), *args],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def write_requests(self, text):
        temp_dir = tempfile.TemporaryDirectory()
        path = Path(temp_dir.name) / "requests.txt"
        path.write_text(text, encoding="utf-8")
        return temp_dir, path

    def test_validate_accepts_sequential_multi_request_file(self):
        temp_dir, request_path = self.write_requests(
            "\n".join(
                [
                    "# W5 serving entry requests",
                    "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=4",
                    (
                        "request_id=req-b prompt_token_ids=81378,37585,374,17 "
                        "decode_steps=2 prefix_cache_required=1"
                    ),
                ]
            )
        )
        with temp_dir:
            result = self.run_entry("--requests", str(request_path), "--validate-only")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "w5_serving_entry: status=valid requests=2 total_decode_steps=6 "
            "entry=nodeA mode=sequential",
            result.stdout,
        )

    def test_print_current_one_shot_env_maps_request_fields(self):
        temp_dir, request_path = self.write_requests(
            (
                "request_id=req-a prompt_token_ids=81378,37585,374 "
                "decode_steps=4 sampler_top_k=20 sampler_seed=7\n"
            )
        )
        with temp_dir:
            result = self.run_entry(
                "--requests",
                str(request_path),
                "--print-current-one-shot-env",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "w5_serving_request: index=0 request_id=req-a prompt_tokens=3 decode_steps=4",
            result.stdout,
        )
        self.assertIn("SIM_W5_SERVING_REQUEST_ID=req-a", result.stdout)
        self.assertIn("SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS=81378,37585,374", result.stdout)
        self.assertIn("SIM_QWEN3_GUEST_DECODE_STEPS=4", result.stdout)
        self.assertIn("SIM_QWEN3_SAMPLER_TOP_K=20", result.stdout)
        self.assertIn("SIM_QWEN3_SAMPLER_SEED=7", result.stdout)

    def test_default_mode_reports_runtime_queue_ready(self):
        temp_dir, request_path = self.write_requests(
            "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=4\n"
        )
        with temp_dir:
            result = self.run_entry("--requests", str(request_path))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("status=ready mode=runtime_queue", result.stdout)

    def test_print_request_lines_normalizes_queue_input(self):
        temp_dir, request_path = self.write_requests(
            (
                "request_id=req-a sampler_seed=7 prompt_token_ids=81378,37585,374 "
                "decode_steps=4 prefix_cache_required=1\n"
            )
        )
        with temp_dir:
            result = self.run_entry(
                "--requests",
                str(request_path),
                "--print-request-lines",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            (
                "request_id=req-a prompt_token_ids=81378,37585,374 "
                "decode_steps=4 sampler_seed=7 prefix_cache_required=1"
            ),
        )

    def test_rejects_invalid_request_file(self):
        temp_dir, request_path = self.write_requests(
            "request_id=req-a request_id=req-b prompt_token_ids=81378 decode_steps=1\n"
        )
        with temp_dir:
            result = self.run_entry("--requests", str(request_path), "--validate-only")

        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate field request_id", result.stderr)

    def test_shell_wrapper_delegates_to_python_entry(self):
        temp_dir, request_path = self.write_requests(
            "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=1\n"
        )
        with temp_dir:
            result = subprocess.run(
                [str(self.entry_sh), "--requests", str(request_path), "--validate-only"],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("w5_serving_entry: status=valid requests=1", result.stdout)

    def test_prints_request_count_and_total_decode_steps_for_runner_integration(self):
        temp_dir, request_path = self.write_requests(
            "\n".join(
                [
                    "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=2",
                    "request_id=req-b prompt_token_ids=81378,37585,374,17 decode_steps=3",
                ]
            )
        )
        with temp_dir:
            count = self.run_entry(
                "--requests", str(request_path), "--print-request-count"
            )
            steps = self.run_entry(
                "--requests", str(request_path), "--print-total-decode-steps"
            )

        self.assertEqual(count.returncode, 0, count.stderr)
        self.assertEqual(steps.returncode, 0, steps.stderr)
        self.assertEqual(count.stdout.strip(), "2")
        self.assertEqual(steps.stdout.strip(), "5")

    def test_prints_first_request_fields_for_nodea_ingress_validation(self):
        temp_dir, request_path = self.write_requests(
            "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=2\n"
        )
        with temp_dir:
            request_id = self.run_entry(
                "--requests", str(request_path), "--print-first-request-id"
            )
            prompt = self.run_entry(
                "--requests", str(request_path), "--print-first-prompt-token-ids"
            )
            steps = self.run_entry(
                "--requests", str(request_path), "--print-first-decode-steps"
            )

        self.assertEqual(request_id.returncode, 0, request_id.stderr)
        self.assertEqual(prompt.returncode, 0, prompt.stderr)
        self.assertEqual(steps.returncode, 0, steps.stderr)
        self.assertEqual(request_id.stdout.strip(), "req-a")
        self.assertEqual(prompt.stdout.strip(), "81378,37585,374")
        self.assertEqual(steps.stdout.strip(), "2")

    def test_submit_dry_run_validates_request_and_cluster_fanout(self):
        with tempfile.TemporaryDirectory(dir="/private/tmp") as tmp:
            tmp_path = Path(tmp)
            env_path = tmp_path / "headless.env"
            env_lines = [
                f"export RUN_DIR='{tmp_path}'",
                "export SIM_W5_SERVING_QUEUE='1'",
            ]
            for node in "ABCDEFGH":
                sock_path = tmp_path / f"node{node}.sock"
                env_lines.append(f"export NODE{node}_SERIAL_SOCKET='{sock_path}'")
            env_path.write_text("\n".join(env_lines) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(self.submit_py),
                    "--env-file",
                    str(env_path),
                    "--request-line",
                    "request_id=req-a prompt_token_ids=81378,37585 decode_steps=2",
                    "--dry-run",
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "would_submit request_id=req-a fanout=cluster targets=8 "
            "wait_targets=fanout wait_nodes=8",
            result.stdout,
        )

    def test_submit_dry_run_supports_nodea_fanout_with_cluster_wait(self):
        with tempfile.TemporaryDirectory(dir="/private/tmp") as tmp:
            tmp_path = Path(tmp)
            env_path = tmp_path / "headless.env"
            env_lines = [
                f"export RUN_DIR='{tmp_path}'",
                "export SIM_W5_SERVING_QUEUE='1'",
            ]
            for node in "ABCDEFGH":
                sock_path = tmp_path / f"node{node}.sock"
                env_lines.append(f"export NODE{node}_SERIAL_SOCKET='{sock_path}'")
            env_path.write_text("\n".join(env_lines) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(self.submit_py),
                    "--env-file",
                    str(env_path),
                    "--request-line",
                    "request_id=req-a prompt_token_ids=81378,37585 decode_steps=2",
                    "--fanout",
                    "nodeA",
                    "--wait-targets",
                    "cluster",
                    "--dry-run",
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "would_submit request_id=req-a fanout=nodeA targets=1 "
            "wait_targets=cluster wait_nodes=8",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
