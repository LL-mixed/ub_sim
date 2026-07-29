#!/usr/bin/env python3
"""Least-connections streaming proxy for replicated DGX ds4 services."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import http.client
import json
import signal
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Iterable


HOP_BY_HOP_HEADERS = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "proxy-connection",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
}


class ProxyError(RuntimeError):
    """Raised when a request cannot be sent to a healthy backend."""


@dataclasses.dataclass
class Backend:
    url: str
    host: str
    port: int
    active: int = 0
    healthy: bool = True
    last_error: str | None = None
    last_check: float | None = None

    @classmethod
    def parse(cls, value: str) -> "Backend":
        parsed = urllib.parse.urlsplit(value)
        if (
            parsed.scheme != "http"
            or not parsed.hostname
            or parsed.path not in ("", "/")
            or parsed.query
            or parsed.fragment
        ):
            raise argparse.ArgumentTypeError(
                "backend must be an HTTP origin such as http://127.0.0.1:8100"
            )
        try:
            port = parsed.port or 80
        except ValueError as error:
            raise argparse.ArgumentTypeError(str(error)) from error
        return cls(
            url=f"http://{parsed.hostname}:{port}",
            host=parsed.hostname,
            port=port,
        )


class BackendPool:
    """Thread-safe least-connections backend state."""

    def __init__(self, backends: Iterable[Backend]) -> None:
        self.backends = list(backends)
        if not self.backends:
            raise ValueError("at least one backend is required")
        self._lock = threading.Lock()
        self._tie_cursor = 0

    def acquire(self) -> Backend:
        with self._lock:
            candidates = [backend for backend in self.backends if backend.healthy]
            if not candidates:
                raise ProxyError("no healthy ds4 backend")
            minimum = min(backend.active for backend in candidates)
            tied = [
                backend for backend in candidates if backend.active == minimum
            ]
            backend = tied[self._tie_cursor % len(tied)]
            self._tie_cursor += 1
            backend.active += 1
            return backend

    def release(self, backend: Backend) -> None:
        with self._lock:
            if backend.active <= 0:
                raise RuntimeError(f"backend active count underflow: {backend.url}")
            backend.active -= 1

    def mark_health(
        self, backend: Backend, healthy: bool, error: str | None = None
    ) -> None:
        with self._lock:
            backend.healthy = healthy
            backend.last_error = error
            backend.last_check = time.time()

    def snapshot(self) -> list[dict[str, object]]:
        with self._lock:
            return [
                {
                    "url": backend.url,
                    "active": backend.active,
                    "healthy": backend.healthy,
                    "last_error": backend.last_error,
                    "last_check": backend.last_check,
                }
                for backend in self.backends
            ]


class ProxyServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        server_address: tuple[str, int],
        pool: BackendPool,
        connect_timeout: float,
        read_timeout: float,
    ) -> None:
        super().__init__(server_address, ProxyHandler)
        self.pool = pool
        self.connect_timeout = connect_timeout
        self.read_timeout = read_timeout


class ProxyHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "dgx-ds4-proxy/1"

    @property
    def proxy_server(self) -> ProxyServer:
        server = self.server
        if not isinstance(server, ProxyServer):
            raise RuntimeError("proxy handler is attached to the wrong server")
        return server

    def do_GET(self) -> None:
        if self.path == "/healthz":
            self._send_health()
            return
        self._proxy()

    def do_POST(self) -> None:
        self._proxy()

    def do_OPTIONS(self) -> None:
        self._proxy()

    def log_message(self, format_string: str, *args: object) -> None:
        sys.stderr.write(
            f"dgx_ds4_proxy: client={self.client_address[0]} "
            f"{format_string % args}\n"
        )

    def _send_health(self) -> None:
        snapshot = self.proxy_server.pool.snapshot()
        healthy = sum(1 for backend in snapshot if backend["healthy"])
        payload = json.dumps(
            {
                "status": "ok" if healthy else "unavailable",
                "healthy_backends": healthy,
                "backends": snapshot,
            },
            separators=(",", ":"),
        ).encode("utf-8")
        self.send_response(200 if healthy else 503)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)
        self.close_connection = True

    def _request_body(self) -> bytes:
        if self.headers.get("Transfer-Encoding"):
            raise ProxyError("chunked client requests are not supported")
        raw_length = self.headers.get("Content-Length", "0")
        try:
            length = int(raw_length)
        except ValueError as error:
            raise ProxyError("invalid Content-Length") from error
        if length < 0:
            raise ProxyError("invalid Content-Length")
        return self.rfile.read(length) if length else b""

    def _upstream_headers(self, backend: Backend) -> dict[str, str]:
        headers: dict[str, str] = {}
        for name, value in self.headers.items():
            lower = name.lower()
            if lower in HOP_BY_HOP_HEADERS or lower in {
                "content-length",
                "expect",
                "host",
            }:
                continue
            headers[name] = value
        headers["Host"] = f"{backend.host}:{backend.port}"
        headers["Connection"] = "close"
        forwarded_for = self.headers.get("X-Forwarded-For")
        client_ip = self.client_address[0]
        headers["X-Forwarded-For"] = (
            f"{forwarded_for}, {client_ip}" if forwarded_for else client_ip
        )
        headers["X-Forwarded-Proto"] = "http"
        return headers

    def _proxy(self) -> None:
        started = time.monotonic()
        backend: Backend | None = None
        upstream: http.client.HTTPConnection | None = None
        response_started = False
        try:
            body = self._request_body()
            backend = self.proxy_server.pool.acquire()
            upstream = http.client.HTTPConnection(
                backend.host,
                backend.port,
                timeout=self.proxy_server.connect_timeout,
            )
            upstream.connect()
            if upstream.sock is not None:
                upstream.sock.settimeout(self.proxy_server.read_timeout)
            upstream.request(
                self.command,
                self.path,
                body=body,
                headers=self._upstream_headers(backend),
            )
            response = upstream.getresponse()
            self.proxy_server.pool.mark_health(backend, True)

            self.send_response(response.status, response.reason)
            has_length = False
            for name, value in response.getheaders():
                lower = name.lower()
                if lower in HOP_BY_HOP_HEADERS:
                    continue
                if lower == "content-length":
                    has_length = True
                self.send_header(name, value)
            self.send_header("X-DS4-Backend", backend.url)
            if not has_length:
                self.send_header("Connection", "close")
            self.end_headers()
            response_started = True

            read_chunk = getattr(response, "read1", response.read)
            while True:
                chunk = read_chunk(64 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except (OSError, http.client.HTTPException, ProxyError) as error:
            if backend is not None:
                self.proxy_server.pool.mark_health(
                    backend, False, f"{type(error).__name__}: {error}"
                )
            if not response_started:
                self._send_proxy_error(error)
        finally:
            if upstream is not None:
                upstream.close()
            if backend is not None:
                self.proxy_server.pool.release(backend)
            self.close_connection = True
            if backend is not None:
                elapsed_ms = (time.monotonic() - started) * 1000.0
                sys.stderr.write(
                    "dgx_ds4_proxy: "
                    f"method={self.command} path={self.path} "
                    f"backend={backend.url} elapsed_ms={elapsed_ms:.3f}\n"
                )

    def _send_proxy_error(self, error: Exception) -> None:
        status = 503 if isinstance(error, ProxyError) else 502
        payload = json.dumps(
            {"error": {"message": str(error), "type": "proxy_error"}},
            separators=(",", ":"),
        ).encode("utf-8")
        with contextlib.suppress(BrokenPipeError, ConnectionResetError):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)


class HealthChecker:
    def __init__(
        self,
        pool: BackendPool,
        path: str,
        interval: float,
        timeout: float,
    ) -> None:
        self.pool = pool
        self.path = path
        self.interval = interval
        self.timeout = timeout
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="dgx-ds4-health",
            daemon=True,
        )

    def start(self) -> None:
        self.check_once()
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=max(self.interval, self.timeout) + 1.0)

    def check_once(self) -> None:
        for backend in self.pool.backends:
            connection = http.client.HTTPConnection(
                backend.host, backend.port, timeout=self.timeout
            )
            try:
                connection.request(
                    "GET",
                    self.path,
                    headers={"Connection": "close"},
                )
                response = connection.getresponse()
                response.read()
                if 200 <= response.status < 500:
                    self.pool.mark_health(backend, True)
                else:
                    self.pool.mark_health(
                        backend, False, f"HTTP {response.status}"
                    )
            except (OSError, http.client.HTTPException) as error:
                self.pool.mark_health(
                    backend, False, f"{type(error).__name__}: {error}"
                )
            finally:
                connection.close()

    def _run(self) -> None:
        while not self._stop.wait(self.interval):
            self.check_once()


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def port_number(value: str) -> int:
    parsed = int(value)
    if parsed < 1 or parsed > 65535:
        raise argparse.ArgumentTypeError("must be between 1 and 65535")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Proxy one ds4 endpoint across replicated DGX backends."
    )
    parser.add_argument("--listen-host", default="0.0.0.0")
    parser.add_argument("--listen-port", type=port_number, default=8000)
    parser.add_argument(
        "--backend",
        action="append",
        type=Backend.parse,
        required=True,
        help="repeat for each backend HTTP origin",
    )
    parser.add_argument(
        "--connect-timeout", type=positive_float, default=5.0
    )
    parser.add_argument("--read-timeout", type=positive_float, default=600.0)
    parser.add_argument("--health-path", default="/v1/models")
    parser.add_argument(
        "--health-interval", type=positive_float, default=5.0
    )
    return parser


def run(args: argparse.Namespace) -> int:
    pool = BackendPool(args.backend)
    server = ProxyServer(
        (args.listen_host, args.listen_port),
        pool,
        args.connect_timeout,
        args.read_timeout,
    )
    checker = HealthChecker(
        pool,
        args.health_path,
        args.health_interval,
        args.connect_timeout,
    )

    def request_stop(signum: int, frame: object) -> None:
        del signum, frame
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    checker.start()
    host, port = server.server_address
    sys.stderr.write(
        "dgx_ds4_proxy: "
        f"listening=http://{host}:{port} "
        f"backends={','.join(backend.url for backend in pool.backends)}\n"
    )
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        checker.stop()
        server.server_close()
    return 0


def main(arguments: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(arguments)
    try:
        return run(args)
    except (OSError, ValueError) as error:
        print(f"dgx_ds4_proxy: status=failed reason={error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
