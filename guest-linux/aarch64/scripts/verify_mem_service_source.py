#!/usr/bin/env python3

import argparse
import subprocess
import sys
from pathlib import Path


REQUIRED_LOCK_KEYS = {"lock_version", "version", "revision"}
REQUIRED_SOURCE_PATHS = (
    "VERSION",
    "apps/mem_service/Makefile",
    "apps/mem_service/mem_service.c",
    "components/mem_service/mem_service.h",
    "components/mem_service/mem_service_qwen3.h",
)


def parse_lock(lock_file: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(lock_file.read_text().splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{lock_file}:{line_number}: expected key=value")
        key, value = line.split("=", 1)
        if key not in REQUIRED_LOCK_KEYS:
            raise ValueError(f"{lock_file}:{line_number}: unknown key {key}")
        if key in values:
            raise ValueError(f"{lock_file}:{line_number}: duplicate key {key}")
        if not value:
            raise ValueError(f"{lock_file}:{line_number}: empty value for {key}")
        values[key] = value
    missing = sorted(REQUIRED_LOCK_KEYS - values.keys())
    if missing:
        raise ValueError(f"{lock_file}: missing keys: {','.join(missing)}")
    if values["lock_version"] != "1":
        raise ValueError(
            f"{lock_file}: unsupported lock_version={values['lock_version']}"
        )
    return values


def git_output(source_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(source_root), *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def verify_source(source_root: Path, lock_file: Path) -> tuple[str, str]:
    if not source_root.is_dir():
        raise ValueError(f"mem_service source checkout not found: {source_root}")
    if not lock_file.is_file():
        raise ValueError(f"mem_service lock file not found: {lock_file}")

    lock = parse_lock(lock_file)
    for relative_path in REQUIRED_SOURCE_PATHS:
        source_path = source_root / relative_path
        if not source_path.is_file():
            raise ValueError(f"incomplete mem_service source: missing {source_path}")

    actual_version = (source_root / "VERSION").read_text().strip()
    if actual_version != lock["version"]:
        raise ValueError(
            "incompatible mem_service source version: "
            f"expected {lock['version']}, got {actual_version or 'missing'}"
        )

    try:
        actual_revision = git_output(source_root, "rev-parse", "HEAD")
        dirty = git_output(source_root, "status", "--porcelain", "--untracked-files=all")
    except (OSError, subprocess.CalledProcessError) as error:
        raise ValueError(
            f"mem_service source must be a readable Git checkout: {source_root}"
        ) from error
    if actual_revision != lock["revision"]:
        raise ValueError(
            "unpinned mem_service source revision: "
            f"expected {lock['revision']}, got {actual_revision}"
        )
    if dirty:
        raise ValueError(
            "mem_service source checkout has uncommitted changes; "
            "commit and update mem_service.lock before downstream validation"
        )
    return actual_version, actual_revision


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify ub_sim's pinned mem_service submodule or override."
    )
    parser.add_argument("--mem-service-root", required=True, type=Path)
    parser.add_argument("--lock-file", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        version, revision = verify_source(
            args.mem_service_root.resolve(),
            args.lock_file.resolve(),
        )
    except ValueError as error:
        print(f"verify_mem_service_source: {error}", file=sys.stderr)
        return 2
    print(
        "mem_service_source_check=ok "
        f"version={version} revision={revision}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
