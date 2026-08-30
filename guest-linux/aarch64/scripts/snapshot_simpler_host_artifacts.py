#!/usr/bin/env python3
"""Create a self-contained snapshot of one Simpler runtime manifest."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys


HOST_ARTIFACT_OUTPUT_LOCK = ".sim-host-artifacts.output.lock"
SNAPSHOT_VERSION = 1


def atomic_write_text(path: Path, contents: str) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(contents, encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def sha256_bytes(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def safe_label(label: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "-", label).strip("-.")
    return value or "artifact"


def snapshot_manifest(source_manifest: Path, output_dir: Path) -> Path:
    source_manifest = source_manifest.expanduser().resolve(strict=True)
    output_dir = output_dir.expanduser().resolve()
    if output_dir.exists():
        raise ValueError(f"snapshot output already exists: {output_dir}")

    lock_path = source_manifest.parent / HOST_ARTIFACT_OUTPUT_LOCK
    with lock_path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_SH)

        source_bytes = source_manifest.read_bytes()
        manifest = json.loads(source_bytes)
        runtime = manifest.get("simpler_runtime")
        if not isinstance(runtime, dict):
            raise ValueError("Simpler manifest has no simpler_runtime object")

        output_dir.mkdir(parents=True)
        copied: dict[Path, Path] = {}
        copy_records: list[dict[str, str | int]] = []

        def snapshot_file(source: str, label: str) -> str:
            source_path = Path(source).expanduser().resolve(strict=True)
            if not source_path.is_file():
                raise ValueError(f"Simpler artifact is not a file: {source_path}")
            existing = copied.get(source_path)
            if existing is not None:
                return str(existing)

            destination = output_dir / (
                f"{len(copied):02d}-{safe_label(label)}-{source_path.name}"
            )
            shutil.copy2(source_path, destination)
            copied[source_path] = destination
            copied_bytes = destination.read_bytes()
            copy_records.append(
                {
                    "label": label,
                    "source": str(source_path),
                    "snapshot": str(destination),
                    "bytes": len(copied_bytes),
                    "sha256": sha256_bytes(copied_bytes),
                }
            )
            return str(destination)

        for key in (
            "host_runtime_library",
            "orch_shared_object",
            "aicpu_binary",
            "aicore_binary",
        ):
            artifact = runtime.get(key)
            if artifact is None:
                continue
            if not isinstance(artifact, dict) or not isinstance(
                artifact.get("source"), str
            ):
                raise ValueError(f"invalid Simpler runtime artifact: {key}")
            artifact["source"] = snapshot_file(artifact["source"], key)

        kernels = runtime.get("kernels", [])
        if not isinstance(kernels, list):
            raise ValueError("Simpler runtime kernels must be a list")
        for index, kernel in enumerate(kernels):
            if not isinstance(kernel, dict):
                raise ValueError(f"invalid Simpler kernel entry: {index}")
            artifact = kernel.get("binary")
            if not isinstance(artifact, dict) or not isinstance(
                artifact.get("source"), str
            ):
                raise ValueError(f"invalid Simpler kernel artifact: {index}")
            artifact["source"] = snapshot_file(
                artifact["source"], f"kernel-{index}"
            )

        runtime_env = runtime.get("runtime_env", {})
        if not isinstance(runtime_env, dict):
            raise ValueError("Simpler runtime_env must be an object")
        for key, value in list(runtime_env.items()):
            if not isinstance(value, str):
                continue
            candidate = Path(value).expanduser()
            if candidate.is_file():
                runtime_env[key] = snapshot_file(value, f"runtime-env-{key}")

        manifest["artifact_snapshot"] = {
            "version": SNAPSHOT_VERSION,
            "source_manifest": str(source_manifest),
            "source_manifest_sha256": sha256_bytes(source_bytes),
            "artifacts": copy_records,
        }
        snapshot_path = output_dir / source_manifest.name
        atomic_write_text(
            snapshot_path,
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        )
        return snapshot_path


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--output-dir", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        snapshot = snapshot_manifest(
            Path(args.manifest), Path(args.output_dir)
        )
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError) as err:
        print(f"artifact snapshot failed: {err}", file=sys.stderr)
        return 2
    print(snapshot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
