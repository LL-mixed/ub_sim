#!/usr/bin/env python3
"""Submit validated W5 serving requests to a running headless cluster."""

from __future__ import annotations

import argparse
import re
import socket
import sys
import time
from pathlib import Path

from w5_serving_entry import (
    RequestFileError,
    load_requests,
    parse_request_line,
    request_to_line,
)


NODE_SOCKET_KEYS = (
    "NODEA_SERIAL_SOCKET",
    "NODEB_SERIAL_SOCKET",
    "NODEC_SERIAL_SOCKET",
    "NODED_SERIAL_SOCKET",
    "NODEE_SERIAL_SOCKET",
    "NODEF_SERIAL_SOCKET",
    "NODEG_SERIAL_SOCKET",
    "NODEH_SERIAL_SOCKET",
)

EXPORT_RE = re.compile(r"^export ([A-Za-z_][A-Za-z0-9_]*)='(.*)'$")


class SubmitError(RuntimeError):
    pass


def load_env_file(path: Path) -> dict[str, str]:
    env: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise SubmitError(f"failed to read env file {path}: {exc}") from exc
    for line in lines:
        match = EXPORT_RE.fullmatch(line.strip())
        if not match:
            continue
        key, value = match.groups()
        env[key] = value.replace("'\\''", "'")
    return env


def request_lines_from_args(args: argparse.Namespace) -> list[str]:
    if args.request_line and args.requests:
        raise SubmitError("--request-line cannot be combined with --requests")
    if args.request_line:
        return [request_to_line(parse_request_line(args.request_line, 1))]
    if args.requests:
        return [request_to_line(request) for request in load_requests(args.requests)]
    raise SubmitError("provide --request-line or --requests")


def socket_paths(env: dict[str, str], fanout: str) -> list[Path]:
    keys = ("NODEA_SERIAL_SOCKET",) if fanout == "nodeA" else NODE_SOCKET_KEYS
    missing = [key for key in keys if not env.get(key)]
    if missing:
        raise SubmitError(f"env file is missing serial sockets: {','.join(missing)}")
    return [Path(env[key]) for key in keys]


def send_line(path: Path, line: str, timeout_s: float) -> None:
    payload = (line + "\n").encode("utf-8")
    if not path.exists():
        raise SubmitError(f"serial socket is missing: {path}")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout_s)
        sock.connect(str(path))
        sock.sendall(payload)


def wait_for_request_done(
    run_dir: Path,
    request_id: str,
    node_count: int,
    timeout_s: float,
) -> None:
    deadline = time.monotonic() + timeout_s
    pattern = f"serving_entry request_done "
    request_pattern = f"request_id={request_id} "
    while time.monotonic() < deadline:
        done = 0
        for log_path in sorted(run_dir.glob("node?_guest.log")):
            try:
                text = log_path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if pattern in text and request_pattern in text:
                done += 1
        if done >= node_count:
            return
        time.sleep(0.25)
    raise SubmitError(
        f"timed out waiting for request_done request_id={request_id} "
        f"nodes={node_count} run_dir={run_dir}"
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Submit W5 serving requests to a running headless cluster."
    )
    parser.add_argument(
        "--env-file",
        required=True,
        type=Path,
        help="headless env file printed by the W5 serve-mode cluster runner",
    )
    parser.add_argument(
        "--request-line",
        help="single request as key=value tokens",
    )
    parser.add_argument(
        "--requests",
        type=Path,
        help="request file; one request per line as key=value tokens",
    )
    parser.add_argument(
        "--fanout",
        choices=("cluster", "nodeA"),
        default="cluster",
        help="serial target fanout; nodeA sends the request only to the cluster entry node",
    )
    parser.add_argument(
        "--wait-targets",
        choices=("fanout", "cluster"),
        default="fanout",
        help="request_done wait scope; cluster waits for all eight nodes",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="socket connection timeout in seconds",
    )
    parser.add_argument(
        "--wait-done",
        action="store_true",
        help="wait until guest logs show request_done on the selected fanout",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate input and print target fanout without writing serial sockets",
    )
    parser.add_argument(
        "--wait-timeout",
        type=float,
        default=600.0,
        help="request_done wait timeout in seconds",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        env = load_env_file(args.env_file)
        lines = request_lines_from_args(args)
        paths = socket_paths(env, args.fanout)
        wait_node_count = (
            len(NODE_SOCKET_KEYS) if args.wait_targets == "cluster" else len(paths)
        )
        for line in lines:
            if not args.dry_run:
                for path in paths:
                    send_line(path, line, args.timeout)
            print(
                "w5_serving_submit: "
                f"{'would_submit' if args.dry_run else 'submitted'} "
                f"request_id={parse_request_line(line, 1).request_id} "
                f"fanout={args.fanout} targets={len(paths)} "
                f"wait_targets={args.wait_targets} wait_nodes={wait_node_count}"
            )
            if args.wait_done and not args.dry_run:
                run_dir = Path(env.get("RUN_DIR", ""))
                if not run_dir:
                    raise SubmitError("env file is missing RUN_DIR")
                wait_for_request_done(
                    run_dir,
                    parse_request_line(line, 1).request_id,
                    wait_node_count,
                    args.wait_timeout,
                )
                print(
                    "w5_serving_submit: "
                    f"request_done request_id={parse_request_line(line, 1).request_id} "
                    f"targets={len(paths)} wait_nodes={wait_node_count}"
                )
    except (RequestFileError, SubmitError, OSError) as exc:
        print(f"w5_serving_submit: status=failed reason={exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
