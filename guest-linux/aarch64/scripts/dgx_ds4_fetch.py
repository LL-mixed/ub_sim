#!/usr/bin/env python3
"""Fetch model and chat data from a DGX-hosted ds4 server."""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import BinaryIO, TextIO


DEFAULT_ENDPOINT = "http://192.168.8.7:8000"
DEFAULT_MODEL = "deepseek-v4-flash"
DEFAULT_BENCHMARK_PROMPT = "The quick brown fox jumps over the lazy dog. "


class FetchError(RuntimeError):
    """Raised when the ds4 endpoint cannot return usable data."""


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fetch data from the three-node DGX ds4 service."
    )
    parser.add_argument(
        "--endpoint",
        default=DEFAULT_ENDPOINT,
        help=f"ds4 HTTP endpoint (default: {DEFAULT_ENDPOINT})",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=600.0,
        help="request timeout in seconds (default: 600)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="write response data to this file instead of stdout",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("models", help="fetch /v1/models")

    chat = subparsers.add_parser("chat", help="call /v1/chat/completions")
    chat.add_argument("--prompt", required=True, help="user prompt")
    chat.add_argument("--model", default=DEFAULT_MODEL, help="model name")
    chat.add_argument(
        "--max-tokens",
        type=positive_int,
        default=128,
        help="maximum generated tokens (default: 128)",
    )
    chat.add_argument(
        "--stream",
        action="store_true",
        help="request SSE streaming and print events as they arrive",
    )
    chat.add_argument(
        "--thinking",
        action="store_true",
        help="enable model thinking output",
    )

    benchmark = subparsers.add_parser(
        "benchmark", help="measure a reproducible TTFT/TPOT baseline"
    )
    prompt_source = benchmark.add_mutually_exclusive_group()
    prompt_source.add_argument("--prompt", help="base prompt to repeat")
    prompt_source.add_argument(
        "--prompt-file", type=Path, help="use the exact UTF-8 prompt in this file"
    )
    benchmark.add_argument(
        "--prompt-repeat",
        type=positive_int,
        default=256,
        help="repeat the built-in or --prompt text this many times (default: 256)",
    )
    benchmark.add_argument("--model", default=DEFAULT_MODEL, help="model name")
    benchmark.add_argument(
        "--max-tokens",
        type=positive_int,
        default=8,
        help="maximum generated tokens per run (default: 8)",
    )
    benchmark.add_argument(
        "--runs", type=positive_int, default=3, help="measured runs (default: 3)"
    )
    benchmark.add_argument(
        "--warmup-runs",
        type=non_negative_int,
        default=1,
        help="unreported warmup runs (default: 1)",
    )
    benchmark.add_argument(
        "--label", default="A", help="configuration label in the report (default: A)"
    )
    benchmark.add_argument(
        "--thinking", action="store_true", help="enable model thinking output"
    )
    return parser


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def non_negative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return parsed


def endpoint_url(endpoint: str, path: str) -> str:
    return f"{endpoint.rstrip('/')}{path}"


def direct_opener() -> urllib.request.OpenerDirector:
    # The operator machine may define a localhost HTTP proxy that cannot reach
    # the private DGX rail. This client must always contact the endpoint directly.
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def open_response(
    request: urllib.request.Request, timeout: float
) -> BinaryIO:
    try:
        return direct_opener().open(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", errors="replace").strip()
        detail = f": {body}" if body else ""
        raise FetchError(f"HTTP {error.code}{detail}") from error
    except urllib.error.URLError as error:
        raise FetchError(f"request failed: {error.reason}") from error
    except TimeoutError as error:
        raise FetchError(f"request timed out after {timeout:g}s") from error


def fetch_models(endpoint: str, timeout: float) -> str:
    request = urllib.request.Request(
        endpoint_url(endpoint, "/v1/models"),
        headers={"Accept": "application/json"},
        method="GET",
    )
    with open_response(request, timeout) as response:
        return format_json(response.read())


def chat_request(
    endpoint: str,
    prompt: str,
    model: str,
    max_tokens: int,
    stream: bool,
    thinking: bool,
) -> urllib.request.Request:
    payload = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0,
        "stream": stream,
        "thinking": thinking,
    }
    return urllib.request.Request(
        endpoint_url(endpoint, "/v1/chat/completions"),
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Accept": "text/event-stream" if stream else "application/json",
            "Content-Type": "application/json",
        },
        method="POST",
    )


def fetch_chat(
    endpoint: str,
    prompt: str,
    model: str,
    max_tokens: int,
    thinking: bool,
    timeout: float,
) -> str:
    request = chat_request(
        endpoint, prompt, model, max_tokens, stream=False, thinking=thinking
    )
    with open_response(request, timeout) as response:
        return format_json(response.read())


def stream_chat(
    endpoint: str,
    prompt: str,
    model: str,
    max_tokens: int,
    thinking: bool,
    timeout: float,
    output: TextIO,
) -> None:
    request = chat_request(
        endpoint, prompt, model, max_tokens, stream=True, thinking=thinking
    )
    with open_response(request, timeout) as response:
        for raw_line in response:
            output.write(raw_line.decode("utf-8", errors="replace"))
            output.flush()


def measure_chat(
    endpoint: str,
    prompt: str,
    model: str,
    max_tokens: int,
    thinking: bool,
    timeout: float,
) -> dict[str, object]:
    request = chat_request(
        endpoint, prompt, model, max_tokens, stream=True, thinking=thinking
    )
    started = time.perf_counter()
    event_times: list[float] = []
    output_parts: list[str] = []
    with open_response(request, timeout) as response:
        for raw_line in response:
            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line.startswith("data:"):
                continue
            data = line.removeprefix("data:").strip()
            if data == "[DONE]":
                break
            try:
                event = json.loads(data)
                content = event["choices"][0]["delta"].get("content")
            except (json.JSONDecodeError, KeyError, IndexError, TypeError) as error:
                raise FetchError(f"invalid SSE event: {data}") from error
            if content:
                event_times.append(time.perf_counter())
                output_parts.append(content)
    finished = time.perf_counter()

    if not event_times:
        raise FetchError("stream completed without a content event")
    ttft_ms = (event_times[0] - started) * 1000.0
    tpot_ms = None
    if len(event_times) > 1:
        tpot_ms = (event_times[-1] - event_times[0]) * 1000.0 / (
            len(event_times) - 1
        )
    return {
        "ttft_ms": round(ttft_ms, 3),
        "tpot_ms": round(tpot_ms, 3) if tpot_ms is not None else None,
        "e2e_ms": round((finished - started) * 1000.0, 3),
        "output_events": len(event_times),
        "output_text": "".join(output_parts),
    }


def benchmark_prompt(args: argparse.Namespace) -> tuple[str, str]:
    if args.prompt_file is not None:
        return args.prompt_file.read_text(encoding="utf-8"), str(args.prompt_file)
    base = args.prompt if args.prompt is not None else DEFAULT_BENCHMARK_PROMPT
    prompt = base * args.prompt_repeat
    prompt += "\nSummarize the text above in one short sentence."
    source = "--prompt" if args.prompt is not None else "built-in"
    return prompt, source


def median_metric(runs: list[dict[str, object]], name: str) -> float | None:
    values = [float(run[name]) for run in runs if run[name] is not None]
    return round(statistics.median(values), 3) if values else None


def run_benchmark(args: argparse.Namespace) -> dict[str, object]:
    prompt, prompt_source = benchmark_prompt(args)
    total_runs = args.warmup_runs + args.runs
    measured: list[dict[str, object]] = []
    for index in range(total_runs):
        is_warmup = index < args.warmup_runs
        phase = "warmup" if is_warmup else "measure"
        phase_index = index + 1 if is_warmup else index - args.warmup_runs + 1
        phase_total = args.warmup_runs if is_warmup else args.runs
        print(
            f"dgx_ds4_fetch: phase={phase} run={phase_index}/{phase_total}",
            file=sys.stderr,
        )
        result = measure_chat(
            args.endpoint,
            prompt,
            args.model,
            args.max_tokens,
            args.thinking,
            args.timeout,
        )
        if not is_warmup:
            result["run"] = phase_index
            measured.append(result)

    return {
        "label": args.label,
        "endpoint": args.endpoint.rstrip("/"),
        "model": args.model,
        "prompt": {
            "source": prompt_source,
            "characters": len(prompt),
            "utf8_bytes": len(prompt.encode("utf-8")),
            "repeat": None if args.prompt_file is not None else args.prompt_repeat,
        },
        "config": {
            "runs": args.runs,
            "warmup_runs": args.warmup_runs,
            "max_tokens": args.max_tokens,
            "thinking": args.thinking,
        },
        "runs": measured,
        "summary": {
            "ttft_ms_median": median_metric(measured, "ttft_ms"),
            "tpot_ms_median": median_metric(measured, "tpot_ms"),
            "e2e_ms_median": median_metric(measured, "e2e_ms"),
        },
        "measurement_note": (
            "TTFT is request start to first content event; TPOT is the mean "
            "interval between ds4 SSE content events."
        ),
    }


def format_json(raw: bytes) -> str:
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FetchError(f"invalid JSON response: {error}") from error
    return json.dumps(data, ensure_ascii=False, indent=2)


def write_text(text: str, output_path: Path | None) -> None:
    if output_path is None:
        print(text)
        return
    output_path.write_text(f"{text.rstrip()}\n", encoding="utf-8", newline="\n")


def run(args: argparse.Namespace) -> None:
    if args.timeout <= 0:
        raise FetchError("timeout must be greater than zero")

    if args.command == "models":
        write_text(fetch_models(args.endpoint, args.timeout), args.output)
        return

    if args.command == "benchmark":
        write_text(
            json.dumps(run_benchmark(args), ensure_ascii=False, indent=2), args.output
        )
        return

    if not args.stream:
        result = fetch_chat(
            args.endpoint,
            args.prompt,
            args.model,
            args.max_tokens,
            args.thinking,
            args.timeout,
        )
        write_text(result, args.output)
        return

    if args.output is None:
        stream_chat(
            args.endpoint,
            args.prompt,
            args.model,
            args.max_tokens,
            args.thinking,
            args.timeout,
            sys.stdout,
        )
        return

    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        stream_chat(
            args.endpoint,
            args.prompt,
            args.model,
            args.max_tokens,
            args.thinking,
            args.timeout,
            output,
        )


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        run(args)
    except (FetchError, OSError) as error:
        print(f"dgx_ds4_fetch: status=failed reason={error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
