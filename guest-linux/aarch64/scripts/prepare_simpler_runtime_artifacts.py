#!/usr/bin/env python3
"""Prepare reusable simpler runtime binaries without compiling a sample program."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path

from prepare_simpler_host_artifacts import (
    default_simpler_root,
    load_simpler_build_api,
    read_runtime_binaries,
    resolve_pto_isa_root,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--runtime-name", default="tensormap_and_ringbuffer")
    parser.add_argument("--simpler-root", default=None)
    parser.add_argument("--pto-isa-root", default=None)
    parser.add_argument("--platform", default="a2a3")
    parser.add_argument("--device-id", type=int, default=0)
    parser.add_argument("--block-dim", type=int, default=3)
    parser.add_argument("--aicpu-thread-num", type=int, default=4)
    args = parser.parse_args()

    simpler_root = Path(args.simpler_root or default_simpler_root()).expanduser().resolve()
    if not simpler_root.exists():
        raise SystemExit(f"simpler root not found: {simpler_root}")
    pto_isa_root = resolve_pto_isa_root(simpler_root, args.pto_isa_root)
    os.environ["PTO_ISA_ROOT"] = str(pto_isa_root)

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    build_dir = output_dir / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    RuntimeBuilder, _, api_kind = load_simpler_build_api(simpler_root)
    builder = RuntimeBuilder(platform=args.platform)
    host_binary, aicpu_binary, aicore_binary, sim_context_binary, simpler_log_binary = read_runtime_binaries(
        builder,
        api_kind,
        args.runtime_name,
        build_dir,
    )

    host_path = output_dir / "runtime_host.bin"
    aicpu_path = output_dir / "runtime_aicpu.bin"
    aicore_path = output_dir / "runtime_aicore.bin"
    host_path.write_bytes(host_binary)
    aicpu_path.write_bytes(aicpu_binary)
    aicore_path.write_bytes(aicore_binary)

    runtime_env: dict[str, str] = {}
    if simpler_log_binary is not None:
        simpler_log_path = output_dir / "libsimpler_log.so"
        simpler_log_path.write_bytes(simpler_log_binary)
        runtime_env["SIMPLER_LOG_LIBRARY"] = str(simpler_log_path)
    if sim_context_binary is not None:
        sim_context_path = output_dir / "libcpu_sim_context.so"
        sim_context_path.write_bytes(sim_context_binary)
        runtime_env["SIMPLER_SIM_CONTEXT_LIBRARY"] = str(sim_context_path)

    manifest = {
        "profile": f"runtime_{args.runtime_name}",
        "runtime_variant": args.runtime_name,
        "simpler_runtime": {
            "host_runtime_library": {
                "id": f"{args.runtime_name}_runtime_host",
                "format": "shared-object",
                "source": str(host_path),
            },
            "aicpu_binary": {
                "id": f"{args.runtime_name}_runtime_aicpu",
                "format": "runtime-binary",
                "source": str(aicpu_path),
            },
            "aicore_binary": {
                "id": f"{args.runtime_name}_runtime_aicore",
                "format": "runtime-binary",
                "source": str(aicore_path),
            },
            "launch": {
                "aicpu_thread_num": args.aicpu_thread_num,
                "block_dim": args.block_dim,
                "device_id": args.device_id,
                "orch_thread_num": 0,
            },
            "runtime_env": runtime_env,
        },
    }

    manifest_path = output_dir / "simpler_runtime_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True))
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
