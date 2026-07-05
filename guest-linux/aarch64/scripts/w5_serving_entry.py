#!/usr/bin/env python3
"""Validate W5 serving-entry request files and runtime queue input."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9._:@+-]+$")
POSITIVE_INT_RE = re.compile(r"^[1-9][0-9]*$")
TOKEN_CSV_RE = re.compile(r"^[0-9]+(,[0-9]+)*$")

REQUIRED_FIELDS = ("request_id", "prompt_token_ids", "decode_steps")
OPTIONAL_ENV_FIELDS = {
    "sampler_top_k": "SIM_QWEN3_SAMPLER_TOP_K",
    "sampler_top_p_milli": "SIM_QWEN3_SAMPLER_TOP_P_MILLI",
    "sampler_temperature_milli": "SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI",
    "sampler_seed": "SIM_QWEN3_SAMPLER_SEED",
    "prefix_cache_required": "SIM_W5_REQUIRE_PREFIX_CACHE",
}
ALLOWED_FIELDS = set(REQUIRED_FIELDS) | set(OPTIONAL_ENV_FIELDS)


@dataclass(frozen=True)
class ServingRequest:
    line_no: int
    fields: dict[str, str]

    @property
    def request_id(self) -> str:
        return self.fields["request_id"]

    @property
    def prompt_token_ids(self) -> str:
        return self.fields["prompt_token_ids"]

    @property
    def decode_steps(self) -> str:
        return self.fields["decode_steps"]

    @property
    def prompt_token_count(self) -> int:
        return len(self.prompt_token_ids.split(","))


class RequestFileError(ValueError):
    pass


def strip_comment(line: str) -> str:
    return line.split("#", 1)[0].strip()


def validate_positive_int(name: str, value: str, line_no: int) -> None:
    if not POSITIVE_INT_RE.fullmatch(value):
        raise RequestFileError(
            f"line {line_no}: {name} must be a positive integer: {value}"
        )


def parse_request_line(line: str, line_no: int) -> ServingRequest:
    fields: dict[str, str] = {}
    for token in line.split():
        if "=" not in token:
            raise RequestFileError(
                f"line {line_no}: expected key=value token, got: {token}"
            )
        key, value = token.split("=", 1)
        if not key or not value:
            raise RequestFileError(
                f"line {line_no}: key=value token must not be empty: {token}"
            )
        if key not in ALLOWED_FIELDS:
            allowed = ",".join(sorted(ALLOWED_FIELDS))
            raise RequestFileError(f"line {line_no}: unsupported field {key}; allowed={allowed}")
        if key in fields:
            raise RequestFileError(f"line {line_no}: duplicate field {key}")
        fields[key] = value

    missing = [key for key in REQUIRED_FIELDS if key not in fields]
    if missing:
        raise RequestFileError(f"line {line_no}: missing required fields: {','.join(missing)}")

    request_id = fields["request_id"]
    if not REQUEST_ID_RE.fullmatch(request_id):
        raise RequestFileError(
            f"line {line_no}: request_id contains unsupported characters: {request_id}"
        )
    if not TOKEN_CSV_RE.fullmatch(fields["prompt_token_ids"]):
        raise RequestFileError(
            f"line {line_no}: prompt_token_ids must be comma-separated unsigned integers"
        )
    validate_positive_int("decode_steps", fields["decode_steps"], line_no)

    for name in (
        "sampler_top_k",
        "sampler_top_p_milli",
        "sampler_temperature_milli",
        "sampler_seed",
    ):
        if name in fields:
            validate_positive_int(name, fields[name], line_no)
    if fields.get("prefix_cache_required") not in (None, "0", "1"):
        raise RequestFileError(
            f"line {line_no}: prefix_cache_required must be 0 or 1"
        )

    return ServingRequest(line_no=line_no, fields=fields)


def load_requests(path: Path) -> list[ServingRequest]:
    requests: list[ServingRequest] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise RequestFileError(f"failed to read request file {path}: {exc}") from exc

    for index, raw_line in enumerate(lines, start=1):
        line = strip_comment(raw_line)
        if not line:
            continue
        requests.append(parse_request_line(line, index))

    if not requests:
        raise RequestFileError(f"request file has no requests: {path}")
    return requests


def print_summary(requests: list[ServingRequest], entry_node: str) -> None:
    print(
        "w5_serving_entry: "
        f"status=valid requests={len(requests)} "
        f"total_decode_steps={total_decode_steps(requests)} "
        f"entry={entry_node} mode=sequential"
    )


def total_decode_steps(requests: list[ServingRequest]) -> int:
    return sum(int(request.decode_steps) for request in requests)


def print_current_one_shot_env(requests: list[ServingRequest], entry_node: str) -> None:
    print_summary(requests, entry_node)
    for index, request in enumerate(requests):
        print(
            "w5_serving_request: "
            f"index={index} request_id={request.request_id} "
            f"prompt_tokens={request.prompt_token_count} "
            f"decode_steps={request.decode_steps}"
        )
        print(f"SIM_W5_SERVING_REQUEST_ID={request.request_id}")
        print(f"SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS={request.prompt_token_ids}")
        print(f"SIM_QWEN3_GUEST_DECODE_STEPS={request.decode_steps}")
        for field_name, env_name in OPTIONAL_ENV_FIELDS.items():
            if field_name in request.fields:
                print(f"{env_name}={request.fields[field_name]}")


def request_to_line(request: ServingRequest) -> str:
    parts = [f"{key}={request.fields[key]}" for key in REQUIRED_FIELDS]
    for field_name in OPTIONAL_ENV_FIELDS:
        if field_name in request.fields:
            parts.append(f"{field_name}={request.fields[field_name]}")
    return " ".join(parts)


def print_request_lines(requests: list[ServingRequest]) -> None:
    for request in requests:
        print(request_to_line(request))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate W5 serving-entry request files."
    )
    parser.add_argument(
        "--requests",
        required=True,
        type=Path,
        help="request file; one request per line as key=value tokens",
    )
    parser.add_argument(
        "--entry-node",
        default="nodeA",
        help="cluster entry node name; default: nodeA",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="only validate the request file",
    )
    parser.add_argument(
        "--print-current-one-shot-env",
        action="store_true",
        help="print the env mapping accepted by the current one-shot W5 path",
    )
    parser.add_argument(
        "--print-request-lines",
        action="store_true",
        help="print normalized request lines accepted by the runtime serving queue",
    )
    parser.add_argument(
        "--print-request-count",
        action="store_true",
        help="print only the number of requests",
    )
    parser.add_argument(
        "--print-total-decode-steps",
        action="store_true",
        help="print only the sum of decode_steps across requests",
    )
    parser.add_argument(
        "--print-first-request-id",
        action="store_true",
        help="print only the first request_id",
    )
    parser.add_argument(
        "--print-first-prompt-token-ids",
        action="store_true",
        help="print only the first request prompt_token_ids",
    )
    parser.add_argument(
        "--print-first-decode-steps",
        action="store_true",
        help="print only the first request decode_steps",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        requests = load_requests(args.requests)
    except RequestFileError as exc:
        print(f"w5_serving_entry: status=invalid reason={exc}", file=sys.stderr)
        return 2

    if args.print_request_count:
        print(len(requests))
        return 0
    if args.print_total_decode_steps:
        print(total_decode_steps(requests))
        return 0
    if args.print_first_request_id:
        print(requests[0].request_id)
        return 0
    if args.print_first_prompt_token_ids:
        print(requests[0].prompt_token_ids)
        return 0
    if args.print_first_decode_steps:
        print(requests[0].decode_steps)
        return 0
    if args.validate_only:
        print_summary(requests, args.entry_node)
        return 0
    if args.print_current_one_shot_env:
        print_current_one_shot_env(requests, args.entry_node)
        return 0
    if args.print_request_lines:
        print_request_lines(requests)
        return 0

    print_summary(requests, args.entry_node)
    print(
        "w5_serving_entry: status=ready "
        "mode=runtime_queue transport=serial fanout=cluster"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
