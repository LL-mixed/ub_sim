#!/usr/bin/env python3
"""Run a W5 Memory Service to Simpler/PTO UB_GM integration probe."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import time
from typing import Any


GUEST_ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = GUEST_ROOT.parents[1]
DEFAULT_RUNNER = GUEST_ROOT / "scripts" / "run_llm_infer_eight_node_guest.sh"
DEFAULT_SNAPSHOTTER = GUEST_ROOT / "scripts" / "snapshot_simpler_host_artifacts.py"
DEFAULT_SIM_CLI = WORKSPACE_ROOT / "target" / "release" / "sim-cli"
EXPECTED_UB_GM_LAYOUT = {
    "profile": "nd",
    "pto_layout": "ND",
    "compute_tile_layout": "RowMajor",
    "rank": 5,
    "shape": [1, 1, 1, 32, 32],
    "strides": [1, 1, 1, 32, 1],
    "global_rows": 32,
    "global_cols": 32,
    "tile_rows": 32,
    "tile_cols": 32,
    "logical_elements": 1024,
    "logical_bytes": 4096,
    "storage_elements": 1024,
    "storage_bytes": 4096,
    "fragments": [{"element_offset": 0, "element_count": 1024}],
}
QWEN_HIDDEN_RANGE_BYTES = 262_144


def positive_int(value: str) -> int:
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def existing_file(value: str) -> pathlib.Path:
    path = pathlib.Path(value).expanduser().resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file does not exist: {path}")
    return path


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the W5 Lingqu Memory Service hidden-state UB_GM probe through "
            "Simpler/PTO TLOAD and TSTORE."
        )
    )
    parser.add_argument("--manifest", required=True, type=existing_file)
    parser.add_argument("--node-count", type=int, choices=(2, 8), default=8)
    parser.add_argument(
        "--profile",
        choices=(
            "qwen3_0_6b_decode",
            "qwen3_14b_decode",
            "deepseek_v4_flash_decode",
        ),
        default="qwen3_0_6b_decode",
    )
    parser.add_argument("--qwen-weights-path", type=pathlib.Path)
    parser.add_argument("--sim-cli-bin", type=pathlib.Path, default=DEFAULT_SIM_CLI)
    parser.add_argument("--runner", type=pathlib.Path, default=DEFAULT_RUNNER)
    parser.add_argument("--snapshotter", type=pathlib.Path, default=DEFAULT_SNAPSHOTTER)
    parser.add_argument("--cna-base", type=positive_int, default=0xF001)
    parser.add_argument("--timeout-ms", type=positive_int, default=300_000)
    parser.add_argument("--decode-steps", type=positive_int, default=2)
    parser.add_argument("--qemu-mem", default="8G")
    parser.add_argument("--qemu-smp", type=positive_int, default=2)
    parser.add_argument("--run-id")
    parser.add_argument("--evidence-dir", type=pathlib.Path)
    parser.add_argument(
        "--print-plan",
        action="store_true",
        help="Print the resolved run plan without building or launching guests.",
    )
    return parser.parse_args(argv)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_json(command: list[str]) -> dict[str, Any]:
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(completed.stdout)


def artifact_fingerprint(sim_cli: pathlib.Path, manifest: pathlib.Path) -> dict[str, Any]:
    payload = command_json(
        [
            str(sim_cli),
            "lingqu-shmem-pto-e2e",
            "--fingerprint-manifest",
            str(manifest),
        ]
    )
    value = payload.get("artifact_fingerprint")
    encoded = payload.get("artifact_fingerprint_hex")
    if payload.get("callable_id") != 1 or not isinstance(value, int) or value <= 0:
        raise RuntimeError("manifest does not describe callable 1")
    if encoded != f"0x{value:016x}":
        raise RuntimeError("artifact fingerprint encodings disagree")
    return payload


def validate_manifest_contract(manifest: pathlib.Path) -> dict[str, Any]:
    try:
        payload = json.loads(manifest.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read host-vector manifest {manifest}: {error}")
    layout = payload.get("ub_gm_layout")
    if not isinstance(layout, dict):
        raise RuntimeError("host-vector manifest has no ub_gm_layout object")
    mismatches = {
        key: {"expected": expected, "actual": layout.get(key)}
        for key, expected in EXPECTED_UB_GM_LAYOUT.items()
        if layout.get(key) != expected
    }
    if mismatches:
        raise RuntimeError(
            "callable 1 requires the canonical 32x32 fp32 UB_GM layout: "
            + json.dumps(mismatches, sort_keys=True)
        )
    runtime = payload.get("simpler_runtime")
    args_template = runtime.get("args_template") if isinstance(runtime, dict) else None
    expected_args = [
        {"kind": "input", "name": "a"},
        {"kind": "input", "name": "b"},
        {"kind": "output", "name": "f"},
    ]
    if args_template != expected_args:
        raise RuntimeError(
            "callable 1 requires args_template input(a), input(b), output(f)"
        )
    kernels = runtime.get("kernels") if isinstance(runtime, dict) else None
    if not isinstance(kernels, list) or len(kernels) != 3:
        raise RuntimeError("callable 1 requires exactly three host-vector kernels")
    return dict(layout)


def snapshot_manifest(snapshotter: pathlib.Path,
                      source: pathlib.Path,
                      output_dir: pathlib.Path) -> pathlib.Path:
    completed = subprocess.run(
        [
            sys.executable,
            str(snapshotter),
            "--manifest",
            str(source),
            "--output-dir",
            str(output_dir),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("artifact snapshotter returned no manifest path")
    manifest = pathlib.Path(lines[-1]).resolve()
    if not manifest.is_file():
        raise RuntimeError(f"artifact snapshot manifest is missing: {manifest}")
    return manifest


def node_ids(node_count: int) -> list[str]:
    return [f"node{chr(ord('A') + index)}" for index in range(node_count)]


def count_matches(path: pathlib.Path, pattern: str) -> int:
    if not path.is_file():
        return 0
    return len(re.findall(pattern, path.read_text(errors="replace").replace("\r", "")))


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def campaign_qemu_leftovers(run_id: str) -> list[int]:
    leftovers: list[int] = []
    for node_id in node_ids(8):
        pid_file = GUEST_ROOT / "out" / f"ub_{node_id}.headless.{run_id}.pid"
        if not pid_file.is_file():
            continue
        try:
            pid = int(pid_file.read_text().strip())
        except ValueError:
            continue
        if process_alive(pid):
            leftovers.append(pid)
    return leftovers


def collect_validation(run_id: str,
                       node_count: int,
                       runner_exit_code: int,
                       fingerprint_before: str,
                       fingerprint_after: str,
                       access_bytes: int,
                       hidden_bytes: int,
                       decode_steps: int,
                       publish_output: bool) -> dict[str, Any]:
    run_dir = GUEST_ROOT / "logs" / f"{run_id}_headless8"
    nodes: list[dict[str, Any]] = []
    all_nodes_pass = True
    tile_count = hidden_bytes // access_bytes if publish_output else 1
    expected_dispatches = tile_count * decode_steps
    for index, node_id in enumerate(node_ids(node_count)):
        guest_log = run_dir / f"{node_id}_guest.log"
        qemu_log = run_dir / f"{node_id}_qemu.log"
        config_count = count_matches(guest_log, r"stage w5_pto_ub_gm_probe_config .*status=ok")
        skip_count = count_matches(guest_log, r"stage w5_pto_ub_gm_probe_skip .*status=skipped")
        submit_count = count_matches(guest_log, r"stage w5_pto_ub_gm_probe_submit .*status=ready")
        semantic_count = count_matches(
            guest_log,
            r"stage w5_pto_ub_gm_probe_semantic .*elements=1024 "
            r"formula=\(2\*x\+1\)\*\(2\*x\+2\) .*status=ok",
        )
        completion_count = count_matches(guest_log, r"stage w5_pto_ub_gm_probe_complete .*status=ok")
        transform_count = count_matches(
            guest_log,
            rf"stage w5_pto_ub_gm_hidden_transform_complete .*bytes={hidden_bytes} "
            rf"tile_bytes={access_bytes} tiles={tile_count} .*status=ok",
        )
        in_place_publish_count = count_matches(
            guest_log,
            rf"stage w5_pto_ub_gm_hidden_publish .*bytes={hidden_bytes} "
            rf".*tiles={tile_count} publish_mode=in_place .*status=ok",
        )
        copied_publish_count = count_matches(
            guest_log,
            r"stage w5_pto_ub_gm_hidden_publish .*tiles=0 "
            r"publish_mode=copy .*status=ok",
        )
        load_count = count_matches(
            qemu_log,
            rf"QEMU_UB_GM_LOAD request=.*length={access_bytes}(?:\s|$)",
        )
        store_count = count_matches(
            qemu_log,
            rf"QEMU_UB_GM_STORE request=.*length={access_bytes}(?:\s|$)",
        )
        zero_staging_count = count_matches(
            qemu_log,
            rf"QEMU_UB_GM_UNBIND .*load_bytes={2 * access_bytes} "
            rf"store_bytes={access_bytes} fences=1 "
            r"segment_payload_staging_bytes=0",
        )
        denied_count = count_matches(
            qemu_log,
            r"QEMU_UB_GM_(?:LOAD|STORE) denied|QEMU_UB_GM_DISPATCH_REJECT",
        )
        if index == 0:
            passed = (
                config_count == 1
                and skip_count == decode_steps
                and load_count == 0
                and store_count == 0
                and denied_count == 0
                and (
                    not publish_output
                    or copied_publish_count == decode_steps
                )
            )
        else:
            passed = (
                config_count == 1
                and submit_count == expected_dispatches
                and semantic_count == expected_dispatches
                and completion_count == expected_dispatches
                and load_count == 2 * expected_dispatches
                and store_count == expected_dispatches
                and zero_staging_count == expected_dispatches
                and denied_count == 0
                and (
                    not publish_output
                    or (
                        transform_count == decode_steps
                        and in_place_publish_count == decode_steps
                    )
                )
            )
        all_nodes_pass = all_nodes_pass and passed
        nodes.append(
            {
                "node": node_id,
                "status": "pass" if passed else "fail",
                "config_count": config_count,
                "skip_count": skip_count,
                "submit_count": submit_count,
                "semantic_count": semantic_count,
                "completion_count": completion_count,
                "transform_count": transform_count,
                "in_place_publish_count": in_place_publish_count,
                "copied_publish_count": copied_publish_count,
                "tload_count": load_count,
                "tstore_count": store_count,
                "zero_staging_count": zero_staging_count,
                "denied_count": denied_count,
                "guest_log": str(guest_log),
                "qemu_log": str(qemu_log),
            }
        )
    leftovers = campaign_qemu_leftovers(run_id)
    fingerprint_stable = fingerprint_before == fingerprint_after
    passed = (
        runner_exit_code == 0
        and all_nodes_pass
        and fingerprint_stable
        and not leftovers
    )
    return {
        "schema": "ub-sim.w5-lingqu-shmem-pto-validation.v1",
        "run_id": run_id,
        "node_count": node_count,
        "access_bytes": access_bytes,
        "hidden_bytes": hidden_bytes,
        "tile_count": tile_count,
        "decode_steps": decode_steps,
        "publish_output": publish_output,
        "status": "pass" if passed else "fail",
        "runner_exit_code": runner_exit_code,
        "artifact_fingerprint_before": fingerprint_before,
        "artifact_fingerprint_after": fingerprint_after,
        "artifact_fingerprint_stable": fingerprint_stable,
        "campaign_qemu_leftovers": leftovers,
        "nodes": nodes,
    }


def resolved_plan(args: argparse.Namespace, run_id: str, evidence_dir: pathlib.Path) -> dict[str, Any]:
    ub_gm_layout = validate_manifest_contract(args.manifest)
    publish_output = args.profile.startswith("qwen3_")
    hidden_bytes = (
        QWEN_HIDDEN_RANGE_BYTES
        if publish_output
        else ub_gm_layout["storage_bytes"]
    )
    return {
        "command": "w5-lingqu-shmem-pto",
        "implementation_stage": (
            "memory_service_hidden_ub_gm_in_place_publish"
            if publish_output
            else "memory_service_hidden_ub_gm_probe"
        ),
        "run_id": run_id,
        "node_count": args.node_count,
        "profile": args.profile,
        "manifest": str(args.manifest),
        "manifest_sha256": sha256_file(args.manifest),
        "ub_gm_layout": ub_gm_layout,
        "access_bytes": ub_gm_layout["storage_bytes"],
        "hidden_bytes": hidden_bytes,
        "tile_count": hidden_bytes // ub_gm_layout["storage_bytes"],
        "publish_output": publish_output,
        "cna_base": args.cna_base,
        "timeout_ms": args.timeout_ms,
        "decode_steps": args.decode_steps,
        "qemu_mem": args.qemu_mem,
        "qemu_smp": args.qemu_smp,
        "runner": str(args.runner.expanduser().resolve()),
        "sim_cli_bin": str(args.sim_cli_bin.expanduser().resolve()),
        "evidence_dir": str(evidence_dir),
        "acceptance_scope": (
            "W5 receives a committed Memory Service hidden ObjectRef, acquires an "
            "OBMM-backed UB_GM memref, executes callable 1 with direct "
            "TLOAD/TSTORE, and publishes the completed local hidden range in place."
        ),
    }


def write_env_file(path: pathlib.Path, values: dict[str, str]) -> None:
    lines = [f"{key}={json.dumps(value)}" for key, value in sorted(values.items())]
    path.write_text("\n".join(lines) + "\n")


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    run_id = args.run_id or time.strftime("w5-lingqu-shmem-pto-%Y%m%dT%H%M%S")
    evidence_dir = (
        args.evidence_dir.expanduser().resolve()
        if args.evidence_dir
        else WORKSPACE_ROOT / "out" / "w5-lingqu-shmem-pto" / run_id
    )
    plan = resolved_plan(args, run_id, evidence_dir)
    if args.print_plan:
        print(json.dumps(plan, indent=2, sort_keys=True))
        return 0
    if evidence_dir.exists():
        raise RuntimeError(f"evidence directory already exists: {evidence_dir}")

    sim_cli = args.sim_cli_bin.expanduser().resolve()
    runner = args.runner.expanduser().resolve()
    snapshotter = args.snapshotter.expanduser().resolve()
    for label, path in (
        ("sim-cli", sim_cli),
        ("runner", runner),
        ("snapshotter", snapshotter),
    ):
        if not path.is_file():
            raise RuntimeError(f"{label} is missing: {path}")
    if args.profile.startswith("qwen3_"):
        if args.qwen_weights_path is None:
            inherited = os.environ.get("SIM_QWEN3_DENSE_WEIGHTS_PATH", "")
            if not inherited:
                raise RuntimeError(
                    "Qwen profile requires --qwen-weights-path or "
                    "SIM_QWEN3_DENSE_WEIGHTS_PATH"
                )
            args.qwen_weights_path = pathlib.Path(inherited)
        args.qwen_weights_path = args.qwen_weights_path.expanduser().resolve()
        if not args.qwen_weights_path.is_dir():
            raise RuntimeError(f"Qwen weights directory is missing: {args.qwen_weights_path}")

    evidence_dir.mkdir(parents=True)
    (evidence_dir / "run-plan.json").write_text(
        json.dumps(plan, indent=2, sort_keys=True) + "\n"
    )
    snapshot = snapshot_manifest(
        snapshotter, args.manifest, evidence_dir / "artifacts"
    )
    snapshot_layout = validate_manifest_contract(snapshot)
    if snapshot_layout != plan["ub_gm_layout"]:
        raise RuntimeError("artifact snapshot changed the UB_GM layout contract")
    fingerprint_before_payload = artifact_fingerprint(sim_cli, snapshot)
    fingerprint_before = fingerprint_before_payload["artifact_fingerprint_hex"]
    (evidence_dir / "artifact-fingerprint-before.json").write_text(
        json.dumps(fingerprint_before_payload, indent=2, sort_keys=True) + "\n"
    )

    runtime_values = {
        "RUN_ID": run_id,
        "SIM_W5_CLUSTER_NODE_COUNT": str(args.node_count),
        "SIM_UAPI_W5_PROFILE": args.profile,
        "SIMPLER_HOST_VECTOR_MANIFEST": str(snapshot),
        "SIMPLER_HOST_VECTOR_MANIFEST_IMMUTABLE": "1",
        "SIM_W5_PTO_UB_GM_PROBE": "1",
        "SIM_W5_PTO_UB_GM_PUBLISH_OUTPUT": (
            "1" if plan["publish_output"] else "0"
        ),
        "SIM_W5_PTO_UB_GM_ARTIFACT_FINGERPRINT": fingerprint_before,
        "SIM_W5_PTO_UB_GM_TIMEOUT_MS": str(args.timeout_ms),
        "SIM_W5_PTO_UB_GM_ACCESS_BYTES": str(plan["access_bytes"]),
        "SIM_W5_PTO_UB_GM_DISABLE_EXPERIMENTAL_GSVA": "1",
        "SIM_LINGQU_SHMEM_PTO_ENABLE": "1",
        "SIM_LINGQU_SHMEM_PTO_CNA_BASE": hex(args.cna_base),
        "SIM_QWEN3_GUEST_DECODE_STEPS": str(args.decode_steps),
        "SIM_QWEN3_DENSE_TP_NODES": str(args.node_count),
        "QEMU_MEM": args.qemu_mem,
        "QEMU_SMP": str(args.qemu_smp),
        "TRACE_FILE": str(evidence_dir / "runner.trace.txt"),
        "RUN_SUMMARY_FILE": str(evidence_dir / "w5-summary.txt"),
    }
    if args.qwen_weights_path:
        runtime_values["SIM_QWEN3_DENSE_WEIGHTS_PATH"] = str(args.qwen_weights_path)
    write_env_file(evidence_dir / "runtime.env", runtime_values)
    runtime_env = os.environ.copy()
    runtime_env.update(runtime_values)

    runner_log = evidence_dir / "runner.log"
    with runner_log.open("w") as stream:
        process = subprocess.Popen(
            ["zsh", str(runner)],
            cwd=WORKSPACE_ROOT,
            env=runtime_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            stream.write(line)
            stream.flush()
            print(line, end="", flush=True)
        runner_exit_code = process.wait()

    fingerprint_after_payload = artifact_fingerprint(sim_cli, snapshot)
    fingerprint_after = fingerprint_after_payload["artifact_fingerprint_hex"]
    (evidence_dir / "artifact-fingerprint-after.json").write_text(
        json.dumps(fingerprint_after_payload, indent=2, sort_keys=True) + "\n"
    )
    validation = collect_validation(
        run_id,
        args.node_count,
        runner_exit_code,
        fingerprint_before,
        fingerprint_after,
        plan["access_bytes"],
        plan["hidden_bytes"],
        args.decode_steps,
        plan["publish_output"],
    )
    validation["source_manifest"] = str(args.manifest)
    validation["source_manifest_sha256"] = sha256_file(args.manifest)
    validation["snapshot_manifest"] = str(snapshot)
    validation["snapshot_manifest_sha256"] = sha256_file(snapshot)
    validation_path = evidence_dir / "validation.json"
    validation_path.write_text(json.dumps(validation, indent=2, sort_keys=True) + "\n")
    print(f"validation.status={validation['status']}")
    print(f"validation.report={validation_path}")
    return 0 if validation["status"] == "pass" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(f"w5-lingqu-shmem-pto: {error}", file=sys.stderr)
        raise SystemExit(2)
