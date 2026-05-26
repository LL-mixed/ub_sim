#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path


W5_RUN_RE = re.compile(r"^.+_w5_(?P<profile>qwen3_[A-Za-z0-9_]+_decode)_[0-9]+$")


@dataclass
class Artifact:
    label: str
    path: Path
    bytes: int


@dataclass
class Run:
    run_id: str
    profile: str
    summary_path: Path | None
    reusable_boundary_source: bool
    artifacts: list[Artifact]


def tree_size(path):
    if not path.exists():
        return 0
    if path.is_file():
        try:
            return path.stat().st_size
        except OSError:
            return 0
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            file_path = Path(root) / name
            try:
                total += file_path.stat().st_size
            except OSError:
                pass
    return total


def format_bytes(value):
    units = ("B", "KiB", "MiB", "GiB")
    current = float(value)
    for unit in units:
        if current < 1024 or unit == units[-1]:
            if unit == "B":
                return f"{int(current)}B"
            return f"{current:.1f}{unit}"
        current /= 1024
    return f"{value}B"


def infer_profile(run_id):
    match = W5_RUN_RE.match(run_id)
    return match.group("profile") if match else "unknown"


def run_id_from_headless_pid_file(path):
    marker = ".headless."
    suffix = ".pid"
    name = path.name
    if not name.startswith("ub_node") or marker not in name or not name.endswith(suffix):
        return ""
    return name.split(marker, 1)[1][: -len(suffix)]


def summary_has_reusable_boundary_coverage(summary_path):
    try:
        text = summary_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return (
        "memory_boundary_observation_summary: " in text
        and "source=w5_guest_range_exit hidden_backend=obmm_shmem" in text
        and "memory_boundary_observation: phase=range_exit " in text
    )


def collect_artifacts(out_dir, logs_dir, run_id, summary_path):
    candidates = [
        ("decision_store", out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json"),
        ("memory_store", out_dir / f"w5_memory_object_store.{run_id}.json"),
        ("memory_store_bin", out_dir / f"w5_memory_object_store.{run_id}.bin"),
        ("object_store_json", out_dir / f"w5_object_service_store.{run_id}.json"),
        ("object_store_bin", out_dir / f"w5_object_service_store.{run_id}.bin"),
        ("engram_state", out_dir / f"w5_memory_engram_state.{run_id}.json"),
        ("registry", out_dir / f"w5_memory_registry.{run_id}"),
        ("headless_env", out_dir / f"headless_eight_node_env.{run_id}.sh"),
        ("headless_cleanup", out_dir / f"headless_eight_node_cleanup.{run_id}.sh"),
        ("initramfs_dir", out_dir / f"initramfs.{run_id}"),
        ("initramfs_image", out_dir / f"initramfs.{run_id}.cpio.gz"),
        ("logs", logs_dir / f"{run_id}_headless8"),
    ]
    if summary_path is not None:
        candidates.insert(0, ("summary", summary_path))
    artifacts = []
    for label, path in candidates:
        if path.exists():
            artifacts.append(Artifact(label=label, path=path, bytes=tree_size(path)))
    for path in sorted(out_dir.glob(f"ub_node*.headless.{run_id}.pid")):
        if path.exists():
            artifacts.append(Artifact(label="headless_pid", path=path, bytes=tree_size(path)))
    return artifacts


def run_ids_from_artifacts(out_dir, logs_dir):
    run_ids = set()
    patterns = (
        ("eight_node_w5_inference_cluster_summary.", ".txt"),
        ("w5_memory_runtime_boundary_lookup.", ".json"),
        ("w5_memory_object_store.", ".json"),
        ("w5_memory_object_store.", ".bin"),
        ("w5_object_service_store.", ".json"),
        ("w5_object_service_store.", ".bin"),
        ("w5_memory_engram_state.", ".json"),
        ("w5_memory_registry.", ""),
        ("headless_eight_node_env.", ".sh"),
        ("headless_eight_node_cleanup.", ".sh"),
        ("initramfs.", ""),
    )
    for prefix, suffix in patterns:
        for path in out_dir.glob(f"{prefix}*_w5_*{suffix}"):
            name = path.name
            run_id = name[len(prefix) :]
            if suffix and run_id.endswith(suffix):
                run_id = run_id[: -len(suffix)]
            if prefix == "initramfs." and run_id.endswith(".cpio.gz"):
                run_id = run_id[: -len(".cpio.gz")]
            if "_w5_" in run_id:
                run_ids.add(run_id)
    for path in logs_dir.glob("*_w5_*_headless8"):
        name = path.name
        if name.endswith("_headless8"):
            run_ids.add(name[: -len("_headless8")])
    for path in out_dir.glob("ub_node*.headless.*_w5_*.pid"):
        run_id = run_id_from_headless_pid_file(path)
        if run_id:
            run_ids.add(run_id)
    return run_ids


def collect_runs(out_dir, logs_dir):
    runs = []
    for run_id in sorted(run_ids_from_artifacts(out_dir, logs_dir)):
        if "_w5_" not in run_id:
            continue
        summary_path = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
        if not summary_path.is_file():
            summary_path = None
        runs.append(
            Run(
                run_id=run_id,
                profile=infer_profile(run_id),
                summary_path=summary_path,
                reusable_boundary_source=(
                    (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").is_file()
                    and summary_path is not None
                    and summary_has_reusable_boundary_coverage(summary_path)
                ),
                artifacts=collect_artifacts(out_dir, logs_dir, run_id, summary_path),
            )
        )
    runs.sort(key=lambda run: run.run_id, reverse=True)
    return runs


def choose_actions(runs, keep_latest, protect_run_ids):
    kept_by_profile = {}
    actions = {}
    reasons = {}
    for run in runs:
        if run.run_id in protect_run_ids:
            actions[run.run_id] = "keep"
            reasons[run.run_id] = "explicit-protect"
            continue
        if run.reusable_boundary_source:
            actions[run.run_id] = "keep"
            reasons[run.run_id] = "reusable-boundary-source"
            continue
        count = kept_by_profile.get(run.profile, 0)
        if count < keep_latest:
            kept_by_profile[run.profile] = count + 1
            actions[run.run_id] = "keep"
            reasons[run.run_id] = f"latest-{keep_latest}-per-profile"
        else:
            actions[run.run_id] = "prune"
            reasons[run.run_id] = f"older-than-latest-{keep_latest}-per-profile"
    return actions, reasons


def ensure_safe_artifact(out_dir, logs_dir, run_id, artifact):
    path = artifact.path.resolve()
    roots = (out_dir.resolve(), logs_dir.resolve())
    if not any(path == root or root in path.parents for root in roots):
        raise RuntimeError(f"artifact is outside W5 roots: {artifact.path}")
    if run_id not in artifact.path.name and run_id not in str(artifact.path):
        raise RuntimeError(f"artifact path does not contain run id: {artifact.path}")


def remove_artifact(artifact):
    if artifact.path.is_dir():
        shutil.rmtree(artifact.path)
    else:
        artifact.path.unlink()


def prune(runs, actions, reasons, out_dir, logs_dir, delete, summary_only):
    total_prune_bytes = 0
    for run in runs:
        run_bytes = sum(artifact.bytes for artifact in run.artifacts)
        action = actions[run.run_id]
        if not summary_only or action == "prune" or run.reusable_boundary_source:
            print(
                "run: "
                f"action={action} reason={reasons[run.run_id]} profile={run.profile} "
                f"reusable_boundary_source={str(run.reusable_boundary_source).lower()} "
                f"bytes={run_bytes} size={format_bytes(run_bytes)} run_id={run.run_id}"
            )
        if action != "prune":
            continue
        total_prune_bytes += run_bytes
        for artifact in run.artifacts:
            ensure_safe_artifact(out_dir, logs_dir, run.run_id, artifact)
            if not summary_only:
                print(
                    "artifact: "
                    f"action={'delete' if delete else 'dry-run'} label={artifact.label} "
                    f"bytes={artifact.bytes} size={format_bytes(artifact.bytes)} "
                    f"path={artifact.path}"
                )
            if delete:
                remove_artifact(artifact)
    return total_prune_bytes


def main(argv):
    parser = argparse.ArgumentParser(
        description="List or prune W5 inference cluster artifacts without deleting reusable boundary sources."
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("guest-linux/aarch64/out"),
        help="W5 out directory containing summaries and stores.",
    )
    parser.add_argument(
        "--logs-dir",
        type=Path,
        default=Path("guest-linux/aarch64/logs"),
        help="W5 logs directory containing headless run logs.",
    )
    parser.add_argument(
        "--keep-latest",
        type=int,
        default=3,
        help="Keep this many newest non-reusable runs per profile.",
    )
    parser.add_argument(
        "--protect-run-id",
        action="append",
        default=[],
        help="Additional run id to keep. May be specified multiple times.",
    )
    parser.add_argument(
        "--delete",
        action="store_true",
        help="Delete prune candidates. Without this flag the command is dry-run only.",
    )
    parser.add_argument(
        "--profile",
        action="append",
        default=[],
        help="Limit pruning to a W5 profile. May be specified multiple times.",
    )
    parser.add_argument(
        "--summary-only",
        action="store_true",
        help="Print run-level decisions and totals without per-artifact lines.",
    )
    args = parser.parse_args(argv)

    if args.keep_latest < 0:
        print("--keep-latest must be non-negative", file=sys.stderr)
        return 2
    if not args.out_dir.is_dir():
        print(f"out dir is missing: {args.out_dir}", file=sys.stderr)
        return 2
    if not args.logs_dir.is_dir():
        print(f"logs dir is missing: {args.logs_dir}", file=sys.stderr)
        return 2

    runs = collect_runs(args.out_dir, args.logs_dir)
    if args.profile:
        profiles = set(args.profile)
        runs = [run for run in runs if run.profile in profiles]
    actions, reasons = choose_actions(runs, args.keep_latest, set(args.protect_run_id))
    prune_bytes = prune(
        runs,
        actions,
        reasons,
        args.out_dir,
        args.logs_dir,
        args.delete,
        args.summary_only,
    )
    print(
        "w5_artifact_prune: "
        f"mode={'delete' if args.delete else 'dry-run'} runs={len(runs)} "
        f"profiles={','.join(args.profile) if args.profile else 'all'} "
        f"prune_candidates={sum(1 for action in actions.values() if action == 'prune')} "
        f"prune_bytes={prune_bytes} prune_size={format_bytes(prune_bytes)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
