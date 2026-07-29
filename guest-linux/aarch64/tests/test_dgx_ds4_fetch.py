from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
import urllib.request
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
CLIENT = REPO_ROOT / "guest-linux" / "aarch64" / "scripts" / "dgx_ds4_fetch.py"


def load_client_module():
    spec = importlib.util.spec_from_file_location("dgx_ds4_fetch", CLIENT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {CLIENT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
        return False


class FakeOpener:
    def __init__(self, response: bytes) -> None:
        self.response = response
        self.requests: list[urllib.request.Request] = []

    def open(self, request: urllib.request.Request, timeout: float):
        self.requests.append(request)
        return FakeResponse(self.response)


class DgxDs4FetchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client = load_client_module()

    def run_client(self, opener: FakeOpener, *arguments: str):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(self.client, "direct_opener", return_value=opener),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            return_code = self.client.main(list(arguments))
        return return_code, stdout.getvalue(), stderr.getvalue()

    def test_direct_opener_disables_environment_proxies(self) -> None:
        with mock.patch.object(
            self.client.urllib.request, "build_opener"
        ) as build_opener:
            self.client.direct_opener()

        handler = build_opener.call_args.args[0]
        self.assertIsInstance(handler, urllib.request.ProxyHandler)
        self.assertEqual(handler.proxies, {})

    def test_models_fetches_default_endpoint(self) -> None:
        opener = FakeOpener(
            json.dumps(
                {"object": "list", "data": [{"id": "deepseek-v4-flash"}]}
            ).encode("utf-8")
        )

        return_code, stdout, stderr = self.run_client(opener, "models")

        self.assertEqual(return_code, 0, stderr)
        self.assertEqual(json.loads(stdout)["data"][0]["id"], "deepseek-v4-flash")
        self.assertEqual(
            opener.requests[0].full_url,
            "http://192.168.8.7:8000/v1/models",
        )
        self.assertEqual(opener.requests[0].method, "GET")

    def test_chat_sends_expected_request(self) -> None:
        opener = FakeOpener(
            json.dumps(
                {
                    "choices": [
                        {"message": {"role": "assistant", "content": "OK"}}
                    ]
                }
            ).encode("utf-8")
        )

        return_code, stdout, stderr = self.run_client(
            opener,
            "chat",
            "--prompt",
            "Reply with OK",
            "--max-tokens",
            "7",
            "--thinking",
        )

        self.assertEqual(return_code, 0, stderr)
        self.assertEqual(
            json.loads(stdout)["choices"][0]["message"]["content"], "OK"
        )
        request = opener.requests[0]
        self.assertEqual(request.full_url, "http://192.168.8.7:8000/v1/chat/completions")
        self.assertEqual(
            json.loads(request.data),
            {
                "model": "deepseek-v4-flash",
                "messages": [{"role": "user", "content": "Reply with OK"}],
                "max_tokens": 7,
                "temperature": 0,
                "stream": False,
                "thinking": True,
            },
        )

    def test_stream_preserves_sse_events(self) -> None:
        opener = FakeOpener(
            (
                'data: {"choices":[{"delta":{"content":"OK"}}]}\n\n'
                "data: [DONE]\n\n"
            ).encode("utf-8")
        )

        return_code, stdout, stderr = self.run_client(
            opener,
            "chat",
            "--prompt",
            "Reply with OK",
            "--stream",
            "--max-tokens",
            "2",
        )

        self.assertEqual(return_code, 0, stderr)
        self.assertIn('"content":"OK"', stdout)
        self.assertTrue(stdout.endswith("data: [DONE]\n\n"))
        self.assertTrue(json.loads(opener.requests[0].data)["stream"])

    def test_output_writes_response_file(self) -> None:
        opener = FakeOpener(json.dumps({"object": "list"}).encode("utf-8"))
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "models.json"

            return_code, stdout, stderr = self.run_client(
                opener, "--output", str(output_path), "models"
            )

            self.assertEqual(return_code, 0, stderr)
            self.assertEqual(stdout, "")
            self.assertEqual(
                json.loads(output_path.read_text(encoding="utf-8"))["object"], "list"
            )

    def test_benchmark_reports_ttft_tpot_and_prompt_shape(self) -> None:
        opener = FakeOpener(
            (
                'data: {"choices":[{"delta":{"role":"assistant"}}]}\n\n'
                'data: {"choices":[{"delta":{"content":"O"}}]}\n\n'
                'data: {"choices":[{"delta":{"content":"K"}}]}\n\n'
                "data: [DONE]\n\n"
            ).encode("utf-8")
        )
        clock = iter([10.0, 10.25, 10.75, 10.8])
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            mock.patch.object(self.client, "direct_opener", return_value=opener),
            mock.patch.object(
                self.client.time, "perf_counter", side_effect=lambda: next(clock)
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            return_code = self.client.main(
                [
                    "benchmark",
                    "--prompt",
                    "abc ",
                    "--prompt-repeat",
                    "2",
                    "--runs",
                    "1",
                    "--warmup-runs",
                    "0",
                    "--max-tokens",
                    "2",
                ]
            )

        self.assertEqual(return_code, 0, stderr.getvalue())
        report = json.loads(stdout.getvalue())
        self.assertEqual(report["label"], "A")
        self.assertEqual(report["prompt"]["source"], "--prompt")
        self.assertEqual(report["prompt"]["repeat"], 2)
        self.assertEqual(report["runs"][0]["ttft_ms"], 250.0)
        self.assertEqual(report["runs"][0]["tpot_ms"], 500.0)
        self.assertEqual(report["runs"][0]["e2e_ms"], 800.0)
        self.assertEqual(report["runs"][0]["output_events"], 2)
        self.assertEqual(report["runs"][0]["output_text"], "OK")
        self.assertIn("phase=measure run=1/1", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
