from __future__ import annotations

import http.client
import importlib.util
import json
import sys
import threading
import time
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
PROXY = REPO_ROOT / "guest-linux" / "aarch64" / "scripts" / "dgx_ds4_proxy.py"


def load_proxy_module():
    spec = importlib.util.spec_from_file_location("dgx_ds4_proxy", PROXY)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {PROXY}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class BackendHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    first_event_sent = threading.Event()
    release_stream = threading.Event()
    last_body = b""

    def log_message(self, format_string: str, *args: object) -> None:
        del format_string, args

    def do_GET(self) -> None:
        payload = json.dumps(
            {"object": "list", "backend_path": self.path}
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        self.close_connection = True

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        type(self).last_body = self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b'data: {"token":"first"}\n\n')
        self.wfile.flush()
        type(self).first_event_sent.set()
        type(self).release_stream.wait(timeout=3.0)
        self.wfile.write(b'data: {"token":"second"}\n\n')
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        self.close_connection = True


class DgxDs4ProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.proxy = load_proxy_module()

    def setUp(self) -> None:
        BackendHandler.first_event_sent.clear()
        BackendHandler.release_stream.clear()
        BackendHandler.last_body = b""
        self.backend_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), BackendHandler
        )
        self.backend_thread = threading.Thread(
            target=self.backend_server.serve_forever, daemon=True
        )
        self.backend_thread.start()
        backend_host, backend_port = self.backend_server.server_address
        backend = self.proxy.Backend.parse(
            f"http://{backend_host}:{backend_port}"
        )
        self.pool = self.proxy.BackendPool([backend])
        self.proxy_server = self.proxy.ProxyServer(
            ("127.0.0.1", 0),
            self.pool,
            connect_timeout=1.0,
            read_timeout=5.0,
        )
        self.proxy_thread = threading.Thread(
            target=self.proxy_server.serve_forever, daemon=True
        )
        self.proxy_thread.start()

    def tearDown(self) -> None:
        BackendHandler.release_stream.set()
        self.proxy_server.shutdown()
        self.proxy_server.server_close()
        self.proxy_thread.join(timeout=2.0)
        self.backend_server.shutdown()
        self.backend_server.server_close()
        self.backend_thread.join(timeout=2.0)

    def proxy_origin(self) -> str:
        host, port = self.proxy_server.server_address
        return f"http://{host}:{port}"

    def test_parser_requires_http_origin_backends(self) -> None:
        parser = self.proxy.build_parser()

        args = parser.parse_args(
            [
                "--backend",
                "http://127.0.0.1:8100",
                "--backend",
                "http://127.0.0.1:8101",
            ]
        )

        self.assertEqual([backend.port for backend in args.backend], [8100, 8101])
        with self.assertRaises(SystemExit):
            parser.parse_args(["--backend", "https://127.0.0.1:8100"])

    def test_pool_uses_least_connections_and_round_robin_ties(self) -> None:
        pool = self.proxy.BackendPool(
            [
                self.proxy.Backend.parse("http://127.0.0.1:8100"),
                self.proxy.Backend.parse("http://127.0.0.1:8101"),
            ]
        )

        first = pool.acquire()
        second = pool.acquire()
        pool.release(first)
        pool.release(second)
        third = pool.acquire()

        self.assertNotEqual(first.url, second.url)
        self.assertEqual(third.url, first.url)
        pool.release(third)

    def test_health_endpoint_reports_backend_state(self) -> None:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        with opener.open(f"{self.proxy_origin()}/healthz", timeout=2.0) as response:
            payload = json.loads(response.read())

        self.assertEqual(response.status, 200)
        self.assertEqual(payload["status"], "ok")
        self.assertEqual(payload["healthy_backends"], 1)
        self.assertEqual(payload["backends"][0]["active"], 0)

    def test_get_forwards_response_and_identifies_backend(self) -> None:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        with opener.open(
            f"{self.proxy_origin()}/v1/models", timeout=2.0
        ) as response:
            payload = json.loads(response.read())
            selected_backend = response.headers["X-DS4-Backend"]

        self.assertEqual(payload["backend_path"], "/v1/models")
        self.assertEqual(selected_backend, self.pool.backends[0].url)

    def test_sse_first_event_is_flushed_without_waiting_for_stream_end(self) -> None:
        host, port = self.proxy_server.server_address
        connection = http.client.HTTPConnection(host, port, timeout=2.0)
        request_body = b'{"stream":true}'
        started = time.monotonic()

        connection.request(
            "POST",
            "/v1/chat/completions",
            body=request_body,
            headers={
                "Content-Type": "application/json",
                "Content-Length": str(len(request_body)),
            },
        )
        response = connection.getresponse()
        first_line = response.readline()
        first_elapsed = time.monotonic() - started

        self.assertTrue(BackendHandler.first_event_sent.is_set())
        self.assertEqual(first_line, b'data: {"token":"first"}\n')
        self.assertLess(first_elapsed, 1.0)
        self.assertEqual(BackendHandler.last_body, request_body)

        BackendHandler.release_stream.set()
        remainder = response.read()
        connection.close()
        self.assertIn(b'data: {"token":"second"}', remainder)
        self.assertIn(b"data: [DONE]", remainder)

    def test_health_checker_marks_connection_failure(self) -> None:
        unreachable = self.proxy.Backend.parse("http://127.0.0.1:1")
        pool = self.proxy.BackendPool([unreachable])
        checker = self.proxy.HealthChecker(
            pool, "/v1/models", interval=10.0, timeout=0.1
        )

        checker.check_once()

        snapshot = pool.snapshot()[0]
        self.assertFalse(snapshot["healthy"])
        self.assertIn("ConnectionRefusedError", snapshot["last_error"])


if __name__ == "__main__":
    unittest.main()
