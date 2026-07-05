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
            "w5_serving_entry: status=valid requests=2 entry=nodeA mode=sequential",
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

    def test_default_mode_refuses_to_pretend_runtime_loop_exists(self):
        temp_dir, request_path = self.write_requests(
            "request_id=req-a prompt_token_ids=81378,37585,374 decode_steps=4\n"
        )
        with temp_dir:
            result = self.run_entry("--requests", str(request_path))

        self.assertEqual(result.returncode, 2)
        self.assertIn("reason=nodeA_serving_request_loop_not_implemented", result.stderr)

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


if __name__ == "__main__":
    unittest.main()
