#!/usr/bin/env python3
import argparse
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from w5_artifact_prune import collect_runs, choose_actions, format_bytes  # noqa: E402
from w5_inference_run_report import build_report  # noqa: E402


def latest_summary_for_profile(out_dir, profile):
    summaries = []
    marker = f"_w5_{profile}_"
    for path in out_dir.glob("eight_node_w5_inference_cluster_summary.*.txt"):
        run_id = path.name.removeprefix("eight_node_w5_inference_cluster_summary.").removesuffix(
            ".txt"
        )
        if marker in run_id:
            summaries.append((run_id, path))
    summaries.sort(reverse=True)
    return summaries[0] if summaries else ("", None)


def compact_reason(text):
    return "; ".join(line.strip() for line in text.splitlines() if line.strip())


def qemu_processes():
    try:
        result = subprocess.run(
            ["pgrep", "-fl", "qemu-system-aarch64"],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        return [], f"pgrep_failed_exception={type(error).__name__}: {error}"
    if result.returncode == 1:
        return [], ""
    if result.returncode != 0:
        try:
            fallback = subprocess.run(
                ["ps", "-axo", "pid=,command="],
                check=False,
                capture_output=True,
                text=True,
            )
        except OSError as error:
            reason = compact_reason(result.stderr) or f"pgrep_failed_rc={result.returncode}"
            return [], f"{reason}; fallback ps_failed_exception={type(error).__name__}: {error}"
        if fallback.returncode == 0:
            qemu_lines = [
                line.strip()
                for line in fallback.stdout.splitlines()
                if "qemu-system-aarch64" in line
            ]
            return qemu_lines, ""
        reason = compact_reason(result.stderr) or f"pgrep_failed_rc={result.returncode}"
        fallback_reason = compact_reason(fallback.stderr) or f"ps_failed_rc={fallback.returncode}"
        return [], f"{reason}; fallback {fallback_reason}"
    return [line for line in result.stdout.splitlines() if line.strip()], ""


def main(argv):
    parser = argparse.ArgumentParser(
        description="Read-only W5 cluster health check for latest run, reusable source, prune state, and QEMU residue."
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
        "--profile",
        default="qwen3_14b_engram_decode",
        help="W5 profile to check.",
    )
    parser.add_argument(
        "--keep-latest",
        type=int,
        default=3,
        help="Retention target used for prune footprint reporting.",
    )
    parser.add_argument(
        "--skip-qemu-check",
        action="store_true",
        help="Skip host QEMU process check. Intended for tests only.",
    )
    parser.add_argument(
        "--require-qemu-check",
        action="store_true",
        help="Fail if the host QEMU process check is unavailable.",
    )
    parser.add_argument(
        "--max-prune-candidates",
        type=int,
        default=None,
        help="Fail if prune candidate count exceeds this limit.",
    )
    parser.add_argument(
        "--max-prune-bytes",
        type=int,
        default=None,
        help="Fail if prune candidate bytes exceed this limit.",
    )
    args = parser.parse_args(argv)

    issues = []
    if not args.out_dir.is_dir():
        issues.append(f"out dir missing: {args.out_dir}")
    if not args.logs_dir.is_dir():
        issues.append(f"logs dir missing: {args.logs_dir}")
    if args.keep_latest < 0:
        issues.append("--keep-latest must be non-negative")
    if args.max_prune_candidates is not None and args.max_prune_candidates < 0:
        issues.append("--max-prune-candidates must be non-negative")
    if args.max_prune_bytes is not None and args.max_prune_bytes < 0:
        issues.append("--max-prune-bytes must be non-negative")
    if issues:
        for issue in issues:
            print(f"issue: {issue}")
        print(f"w5_health_check: status=fail profile={args.profile}")
        return 1

    latest_run_id, latest_summary = latest_summary_for_profile(args.out_dir, args.profile)
    latest_report = None
    if latest_summary is None:
        issues.append(f"no W5 summary found for profile={args.profile}")
    else:
        latest_report = build_report(latest_summary)
        if latest_report["status"] != "pass":
            issues.append(f"latest summary report failed run_id={latest_run_id}")
            issues.extend(latest_report["issues"])

    runs = [run for run in collect_runs(args.out_dir, args.logs_dir) if run.profile == args.profile]
    reusable = [run for run in runs if run.reusable_boundary_source]
    if not reusable:
        issues.append(f"no reusable boundary source found for profile={args.profile}")
    reusable.sort(key=lambda run: run.run_id, reverse=True)
    actions, _reasons = choose_actions(runs, args.keep_latest, set())
    prune_candidates = sum(1 for action in actions.values() if action == "prune")
    prune_bytes = sum(
        sum(artifact.bytes for artifact in run.artifacts)
        for run in runs
        if actions.get(run.run_id) == "prune"
    )
    if (
        args.max_prune_candidates is not None
        and prune_candidates > args.max_prune_candidates
    ):
        issues.append(
            "prune candidate count exceeds limit: "
            f"actual={prune_candidates} limit={args.max_prune_candidates}"
        )
    if args.max_prune_bytes is not None and prune_bytes > args.max_prune_bytes:
        issues.append(
            "prune footprint exceeds limit: "
            f"actual_bytes={prune_bytes} limit_bytes={args.max_prune_bytes} "
            f"actual_size={format_bytes(prune_bytes)}"
        )

    qemu_unavailable = ""
    if args.skip_qemu_check:
        qemu_lines = []
    else:
        qemu_lines, qemu_unavailable = qemu_processes()
    if qemu_lines:
        issues.append(f"qemu residue detected count={len(qemu_lines)}")
    if qemu_unavailable and args.require_qemu_check:
        issues.append(f"qemu residue check unavailable: {qemu_unavailable}")

    print(
        "latest_summary: "
        f"run_id={latest_run_id or 'none'} "
        f"status={latest_report['status'] if latest_report else 'missing'} "
        f"path={latest_summary if latest_summary else 'none'}"
    )
    if latest_report:
        timing = latest_report["timing"]
        shortpath = latest_report["shortpath"]
        print(
            "latest_shortpath: "
            f"lookup_hits={shortpath['lookup_hits']} "
            f"actual_range_forwards={shortpath['actual_range_forwards']} "
            f"actual_runtime_inputs={shortpath['actual_runtime_inputs']} "
            f"actual_runtime_outputs={shortpath['actual_runtime_outputs']} "
            f"shortpath_no_dispatch={shortpath['shortpath_no_dispatch']} "
            f"round_sum_ms={timing['round_sum_ms']} "
            f"avg_round_ms={timing['avg_round_ms']}"
        )
    print(
        "reusable_source: "
        f"count={len(reusable)} latest={reusable[0].run_id if reusable else 'none'}"
    )
    print(
        "prune_footprint: "
        f"runs={len(runs)} keep_latest={args.keep_latest} "
        f"prune_candidates={prune_candidates} "
        f"prune_bytes={prune_bytes} prune_size={format_bytes(prune_bytes)}"
    )
    if qemu_unavailable:
        print(f"qemu_residue: unavailable reason={qemu_unavailable}")
    else:
        print(f"qemu_residue: count={len(qemu_lines)}")
    for line in qemu_lines:
        print(f"qemu_residue_line: {line}")
    for issue in issues:
        print(f"issue: {issue}")
    print(
        "w5_health_check: "
        f"status={'fail' if issues else 'pass'} profile={args.profile}"
    )
    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
