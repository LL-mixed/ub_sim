#!/usr/bin/env python3
"""Compatibility CLI for building HostMatmul simpler artifacts."""

from __future__ import annotations

import sys
from pathlib import Path

import prepare_simpler_host_artifacts


def main() -> int:
    output_dir = None
    passthrough = []
    args = iter(sys.argv[1:])
    for arg in args:
        if arg == "--output-dir":
            output_dir = next(args)
        else:
            passthrough.append(arg)
    if output_dir is None and passthrough and not passthrough[0].startswith("-"):
        output_dir = passthrough.pop(0)
    if output_dir is None:
        output_dir = "/tmp/simpler-host-matmul-artifacts"
    sys.argv = [
        str(Path(__file__)),
        "--profile",
        "host_matmul",
        "--output-dir",
        output_dir,
        *passthrough,
    ]
    return prepare_simpler_host_artifacts.main()


if __name__ == "__main__":
    raise SystemExit(main())
