#!/usr/bin/env python3
"""Compare direct-EL0 coroutine and Linux-task remote-load scheduling."""

from __future__ import annotations

import argparse
import copy
import csv
import json
import shlex
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any


SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[3]
DEFAULT_RUNNER = REPO_ROOT / "guest-linux/aarch64/scripts/run_ub_obmm_eval.sh"
MODES = ("el0-coroutine", "kernel-task")


def parse_integer_list(value: str, option: str, *, allow_zero: bool = False) -> list[int]:
    values: list[int] = []
    for item in value.split(","):
        try:
            parsed = int(item)
        except ValueError as error:
            raise argparse.ArgumentTypeError(f"{option} entries must be integers") from error
        if parsed < 0 or (parsed == 0 and not allow_zero):
            qualifier = "non-negative" if allow_zero else "positive"
            raise argparse.ArgumentTypeError(f"{option} entries must be {qualifier}")
        values.append(parsed)
    if not values:
        raise argparse.ArgumentTypeError(f"{option} must not be empty")
    if len(set(values)) != len(values):
        raise argparse.ArgumentTypeError(f"{option} contains duplicate entries")
    return values


def fnv1a64(payload: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in payload:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{value:016x}"


def build_model_manifest(base: dict[str, Any], latency_us: int, seed: int) -> dict[str, Any]:
    manifest = copy.deepcopy(base)
    model = manifest["remote_memory_model"]
    model["enabled"] = True
    model["time_source"] = "qemu_virtual"
    model["fixed_latency_ns"] = latency_us * 1_000
    model["jitter"] = {"mode": "none", "max_abs_ns": 0}
    model["tail"] = {"probability_ppm": 0, "extra_latency_ns": 0}
    model["drop_ppm"] = 0
    model["error_ppm"] = 0
    model["duplicate_ppm"] = 0
    model["seed"] = seed
    payload = {
        "schema": manifest["schema"],
        "scenario_name": manifest["scenario_name"],
        "scenario_seed": manifest["scenario_seed"],
        "remote_memory_model": model,
    }
    canonical = json.dumps(
        payload, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    manifest["manifest_hash"] = fnv1a64(canonical)
    return manifest


def write_json(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=False) + "\n"
    )


def preserve_failed_log(path: Path) -> Path:
    attempt = 1
    while True:
        preserved = path.with_name(
            f"{path.stem}.attempt-{attempt}{path.suffix}"
        )
        if not preserved.exists():
            path.rename(preserved)
            return preserved
        attempt += 1


def summary_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def find_unique_line(text: str, prefix: str) -> str:
    lines = [line.strip() for line in text.splitlines() if line.startswith(prefix)]
    unique = list(dict.fromkeys(lines))
    if len(unique) != 1:
        raise ValueError(
            f"expected one distinct {prefix} line, found {len(unique)}"
        )
    return unique[0]


def mode_order(latency_index: int, seed_index: int) -> tuple[str, str]:
    if (latency_index + seed_index) % 2:
        return (MODES[1], MODES[0])
    return MODES


def async_load_model_from_scenario(path: Path) -> str:
    fields: dict[str, str] = {}
    in_block = False
    for line in path.read_text().splitlines():
        if line == "async_load_model:":
            in_block = True
            continue
        if not in_block:
            continue
        if line and not line.startswith("  "):
            break
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or ":" not in stripped:
            continue
        key, value = stripped.split(":", 1)
        fields[key] = value.strip()
    required = (
        "enabled",
        "context_entries",
        "pending_load_entries",
        "event_queue_depth",
        "clock_mhz",
    )
    missing = [field for field in required if field not in fields]
    if missing:
        raise ValueError(f"{path}: async_load_model lacks {', '.join(missing)}")
    enabled = {"true": "1", "false": "0"}.get(fields["enabled"])
    if enabled is None:
        raise ValueError(f"{path}: async_load_model.enabled must be true or false")
    return (
        f"v3|enabled={enabled}|contexts={fields['context_entries']}|"
        f"pending={fields['pending_load_entries']}|"
        f"events={fields['event_queue_depth']}|clock_mhz={fields['clock_mhz']}"
    )


def scenario_identity(path: Path) -> tuple[str, int]:
    fields: dict[str, str] = {}
    in_block = False
    for line in path.read_text().splitlines():
        if line == "scenario:":
            in_block = True
            continue
        if not in_block:
            continue
        if line and not line.startswith("  "):
            break
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or ":" not in stripped:
            continue
        key, value = stripped.split(":", 1)
        fields[key] = value.strip()
    if not fields.get("name") or not fields.get("seed"):
        raise ValueError(f"{path}: scenario name or seed is absent")
    try:
        seed = int(fields["seed"])
    except ValueError as error:
        raise ValueError(f"{path}: scenario seed is not an integer") from error
    return fields["name"], seed


def make_guest_args(
    mode: str, contexts: int, operations: int, latency_us: int, seed: int,
    deadline_us: int,
) -> str:
    parts = [
        "--mode", "async-load",
        "--async-load-producer-consumer",
        "--async-load-completion", "replay",
        "--async-load-event-log", "off",
    ]
    if mode == "el0-coroutine":
        parts.extend(["--coroutines", str(contexts)])
    elif mode == "kernel-task":
        parts.extend(["--kernel-task-replay", "--threads", str(contexts)])
    else:
        raise ValueError(f"unsupported mode: {mode}")
    parts.extend(
        [
            "--inflight", "1",
            "--lookahead", "0",
            "--access-bytes", "8",
            "--pattern", "sequential",
            "--compute-us", "0",
            "--iterations", str(operations),
            "--warmup", "0",
            "--trace-sample-ppm", "0",
            "--deadline-us", str(deadline_us),
            "--seed", str(seed),
            "--expected-outcome", "success",
            "--producer-index", "0",
            "--verify",
        ]
    )
    return " ".join(parts)


def make_command(
    runner: Path, scenario: Path, manifest: Path, run_id: str,
    async_load_model: str, timeout_sec: int, guest_args: str,
) -> list[str]:
    return [
        "zsh",
        str(runner),
        "--node-count",
        "2",
        "--scenario-config",
        str(scenario),
        "--remote-memory-model-manifest",
        str(manifest),
        "--async-load-model",
        async_load_model,
        "--run-id",
        run_id,
        "--timeout-sec",
        str(timeout_sec),
        "--obmm-async-args",
        guest_args,
    ]


def parse_case(
    text: str, mode: str, latency_us: int, seed: int, contexts: int,
    operations: int, log_path: Path,
) -> dict[str, Any]:
    prefix = (
        "OBMM_ASYNC_LOAD_SUMMARY "
        if mode == "el0-coroutine"
        else "OBMM_ASYNC_LOAD_KERNEL_TASK_SUMMARY "
    )
    summary_line = find_unique_line(text, prefix)
    run_line = find_unique_line(text, "OBMM_RUN_EVIDENCE ")
    summary = summary_fields(summary_line)
    evidence = summary_fields(run_line)
    context_field = "coroutines" if mode == "el0-coroutine" else "threads"
    expected = {
        "status": "pass",
        "event_log": "off",
        "operations": str(operations),
        context_field: str(contexts),
        "replay_consumed": str(operations),
        "replay_mismatch": "0",
    }
    for field, value in expected.items():
        if summary.get(field) != value:
            raise ValueError(
                f"{log_path}: summary {field}={summary.get(field)!r}, expected {value!r}"
            )
    if mode == "el0-coroutine":
        if summary.get("async_load_completion") != "replay":
            raise ValueError(f"{log_path}: direct-EL0 retirement is not replay")
        if summary.get("values_verified") != str(operations):
            raise ValueError(f"{log_path}: direct-EL0 verification count mismatch")
    else:
        if summary.get("retirement") != "replay":
            raise ValueError(f"{log_path}: kernel-task retirement is not replay")
        if summary.get("verified") != str(operations):
            raise ValueError(f"{log_path}: kernel-task verification count mismatch")
        if summary.get("direct_el0_upcalls") != "0":
            raise ValueError(f"{log_path}: kernel-task mode used a direct EL0 upcall")
    numeric_fields = (
        "guest_ns_p50",
        "guest_ns_p95",
        "guest_ns_p99",
        "guest_ns_max",
        "makespan_ns",
    )
    row: dict[str, Any] = {
        "mode": mode,
        "latency_us": latency_us,
        "seed": seed,
        "contexts": contexts,
        "operations": operations,
        "checksum": summary.get("checksum", ""),
        "model_contract_hash": evidence.get("model_contract_hash", ""),
        "scenario_sha256": evidence.get("scenario_sha256", ""),
        "qemu_sha256": evidence.get("qemu_sha256", ""),
        "kernel_sha256": evidence.get("kernel_sha256", ""),
        "initramfs_sha256": evidence.get("initramfs_sha256", ""),
        "log": str(log_path),
    }
    for field in numeric_fields:
        try:
            row[field] = int(summary[field])
        except (KeyError, ValueError) as error:
            raise ValueError(f"{log_path}: invalid {field}") from error
    if row["makespan_ns"] <= 0:
        raise ValueError(f"{log_path}: non-positive makespan")
    row["operations_per_second"] = operations * 1_000_000_000 / row["makespan_ns"]
    return row


def validate_pairs(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, int], dict[str, dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault((row["latency_us"], row["seed"]), {})[row["mode"]] = row
    pairs: list[dict[str, Any]] = []
    artifact_fields = (
        "model_contract_hash",
        "scenario_sha256",
        "qemu_sha256",
        "kernel_sha256",
        "initramfs_sha256",
    )
    for (latency_us, seed), by_mode in sorted(grouped.items()):
        if set(by_mode) != set(MODES):
            raise ValueError(f"latency={latency_us} seed={seed}: incomplete mode pair")
        direct = by_mode["el0-coroutine"]
        kernel = by_mode["kernel-task"]
        for field in artifact_fields:
            if direct[field] != kernel[field]:
                raise ValueError(
                    f"latency={latency_us} seed={seed}: pair differs in {field}"
                )
        if direct["checksum"] != kernel["checksum"]:
            raise ValueError(f"latency={latency_us} seed={seed}: checksum mismatch")
        pairs.append(
            {
                "latency_us": latency_us,
                "seed": seed,
                "el0_coroutine_makespan_ns": direct["makespan_ns"],
                "kernel_task_makespan_ns": kernel["makespan_ns"],
                "kernel_over_el0_makespan_ratio": (
                    kernel["makespan_ns"] / direct["makespan_ns"]
                ),
                "el0_coroutine_guest_ns_p50": direct["guest_ns_p50"],
                "kernel_task_guest_ns_p50": kernel["guest_ns_p50"],
                "kernel_over_el0_p50_ratio": (
                    kernel["guest_ns_p50"] / direct["guest_ns_p50"]
                    if direct["guest_ns_p50"]
                    else None
                ),
                "el0_coroutine_guest_ns_p95": direct["guest_ns_p95"],
                "kernel_task_guest_ns_p95": kernel["guest_ns_p95"],
                "kernel_over_el0_p95_ratio": (
                    kernel["guest_ns_p95"] / direct["guest_ns_p95"]
                    if direct["guest_ns_p95"]
                    else None
                ),
                "el0_coroutine_guest_ns_p99": direct["guest_ns_p99"],
                "kernel_task_guest_ns_p99": kernel["guest_ns_p99"],
                "kernel_over_el0_p99_ratio": (
                    kernel["guest_ns_p99"] / direct["guest_ns_p99"]
                    if direct["guest_ns_p99"]
                    else None
                ),
            }
        )
    return pairs


def validate_campaign_artifacts(rows: list[dict[str, Any]]) -> dict[str, str]:
    artifact_fields = (
        "scenario_sha256",
        "qemu_sha256",
        "kernel_sha256",
        "initramfs_sha256",
    )
    fingerprints: dict[str, str] = {}
    for field in artifact_fields:
        values = {row[field] for row in rows if row[field]}
        if len(values) != 1:
            raise ValueError(
                f"campaign has {len(values)} distinct non-empty {field} values"
            )
        fingerprints[field] = values.pop()
    return fingerprints


def aggregate_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, str], list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault((row["latency_us"], row["mode"]), []).append(row)
    aggregate: list[dict[str, Any]] = []
    for (latency_us, mode), values in sorted(grouped.items()):
        makespans = [value["makespan_ns"] for value in values]
        p50_values = [value["guest_ns_p50"] for value in values]
        p95_values = [value["guest_ns_p95"] for value in values]
        p99_values = [value["guest_ns_p99"] for value in values]
        throughputs = [value["operations_per_second"] for value in values]
        aggregate.append(
            {
                "latency_us": latency_us,
                "mode": mode,
                "seeds": len(values),
                "makespan_ns_median": int(statistics.median(makespans)),
                "guest_ns_p50_median": int(statistics.median(p50_values)),
                "guest_ns_p95_median": int(statistics.median(p95_values)),
                "guest_ns_p99_median": int(statistics.median(p99_values)),
                "operations_per_second_median": statistics.median(throughputs),
                "makespan_ns_min": min(makespans),
                "makespan_ns_max": max(makespans),
            }
        )
    return aggregate


def aggregate_pairs(pairs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[int, list[dict[str, Any]]] = {}
    for pair in pairs:
        grouped.setdefault(pair["latency_us"], []).append(pair)
    aggregate: list[dict[str, Any]] = []
    for latency_us, values in sorted(grouped.items()):
        makespan_ratios = [
            value["kernel_over_el0_makespan_ratio"] for value in values
        ]
        p99_ratios = [
            value["kernel_over_el0_p99_ratio"] for value in values
            if value["kernel_over_el0_p99_ratio"] is not None
        ]
        p50_ratios = [
            value["kernel_over_el0_p50_ratio"] for value in values
            if value["kernel_over_el0_p50_ratio"] is not None
        ]
        p95_ratios = [
            value["kernel_over_el0_p95_ratio"] for value in values
            if value["kernel_over_el0_p95_ratio"] is not None
        ]
        aggregate.append(
            {
                "latency_us": latency_us,
                "seeds": len(values),
                "kernel_over_el0_makespan_ratio_median": statistics.median(
                    makespan_ratios
                ),
                "kernel_over_el0_makespan_ratio_min": min(makespan_ratios),
                "kernel_over_el0_makespan_ratio_max": max(makespan_ratios),
                "kernel_over_el0_p50_ratio_median": statistics.median(
                    p50_ratios
                ),
                "kernel_over_el0_p95_ratio_median": statistics.median(
                    p95_ratios
                ),
                "kernel_over_el0_p99_ratio_median": statistics.median(
                    p99_ratios
                ),
            }
        )
    return aggregate


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a paired trace-off replay comparison of direct EL0 coroutine "
            "scheduling and Linux-task scheduling."
        )
    )
    parser.add_argument("--scenario-config", required=True, type=Path)
    parser.add_argument("--base-model-manifest", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--latencies-us", default="1,10,100,1000,10000")
    parser.add_argument("--seeds", default="1,2,3")
    parser.add_argument("--contexts", type=int, default=4)
    parser.add_argument("--operations", type=int, default=256)
    parser.add_argument("--deadline-us", type=int)
    parser.add_argument("--timeout-sec", type=int, default=300)
    parser.add_argument(
        "--async-load-model",
        help="override the async-load model derived from --scenario-config",
    )
    parser.add_argument("--runner", type=Path, default=DEFAULT_RUNNER)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    args.latencies_us = parse_integer_list(
        args.latencies_us, "--latencies-us", allow_zero=True
    )
    args.seeds = parse_integer_list(args.seeds, "--seeds")
    if args.contexts < 2 or args.contexts > 64:
        parser.error("--contexts must be in [2, 64]")
    if args.operations < args.contexts or args.operations % args.contexts:
        parser.error("--operations must be a multiple of --contexts")
    if args.deadline_us is not None and args.deadline_us <= 0:
        parser.error("--deadline-us must be positive")
    if args.timeout_sec <= 0:
        parser.error("--timeout-sec must be positive")
    for path, option in (
        (args.scenario_config, "--scenario-config"),
        (args.base_model_manifest, "--base-model-manifest"),
        (args.runner, "--runner"),
    ):
        if not path.is_file():
            parser.error(f"{option} does not exist: {path}")
    if args.async_load_model is None:
        args.async_load_model = async_load_model_from_scenario(args.scenario_config)
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    output_dir = args.output_dir.resolve()
    if output_dir.exists() and any(output_dir.iterdir()) and not args.resume:
        raise ValueError(f"output directory is not empty; use --resume: {output_dir}")
    models_dir = output_dir / "models"
    logs_dir = output_dir / "logs"
    models_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)
    base = json.loads(args.base_model_manifest.read_text())
    for required in ("schema", "scenario_name", "scenario_seed", "remote_memory_model"):
        if required not in base:
            raise ValueError(f"base model manifest lacks {required}")
    base["scenario_name"], base["scenario_seed"] = scenario_identity(
        args.scenario_config
    )

    planned: list[dict[str, Any]] = []
    campaign_tag = fnv1a64(str(output_dir).encode("utf-8")).split(":", 1)[1][:6]
    for latency_index, latency_us in enumerate(args.latencies_us):
        for seed_index, seed in enumerate(args.seeds):
            model = build_model_manifest(base, latency_us, seed)
            manifest_path = models_dir / f"latency-{latency_us}us-seed-{seed}.json"
            write_json(manifest_path, model)
            deadline_us = args.deadline_us or max(100_000, latency_us * 20)
            for mode in mode_order(latency_index, seed_index):
                mode_tag = "el0" if mode == "el0-coroutine" else "kt"
                run_id = (
                    f"alc-{campaign_tag}-l{latency_us}-c{args.contexts}-"
                    f"o{args.operations}-s{seed}-{mode_tag}"
                )
                guest_args = make_guest_args(
                    mode, args.contexts, args.operations, latency_us, seed,
                    deadline_us,
                )
                command = make_command(
                    args.runner.resolve(), args.scenario_config.resolve(),
                    manifest_path, run_id, args.async_load_model,
                    args.timeout_sec, guest_args,
                )
                planned.append(
                    {
                        "mode": mode,
                        "latency_us": latency_us,
                        "seed": seed,
                        "model_manifest": str(manifest_path),
                        "model_contract_hash": model["manifest_hash"],
                        "run_id": run_id,
                        "command": command,
                    }
                )
    run_manifest = {
        "schema": 1,
        "comparison": "direct-el0-coroutine-vs-kernel-task",
        "completion": "replay",
        "event_log": "off",
        "event_trace_callback": "off",
        "contexts": args.contexts,
        "operations": args.operations,
        "latencies_us": args.latencies_us,
        "seeds": args.seeds,
        "cases": planned,
    }
    write_json(output_dir / "run-manifest.json", run_manifest)
    if args.dry_run:
        for case in planned:
            print("DRY_RUN", shlex.join(case["command"]))
        write_json(
            output_dir / "validation.json",
            {"schema": 1, "status": "dry-run", "completed": 0, "planned": len(planned)},
        )
        return 0

    rows: list[dict[str, Any]] = []
    for case_index, case in enumerate(planned, 1):
        log_path = logs_dir / f"{case['run_id']}.log"
        row: dict[str, Any] | None = None
        if log_path.is_file() and args.resume:
            output = log_path.read_text(errors="replace")
            try:
                row = parse_case(
                    output, case["mode"], case["latency_us"], case["seed"],
                    args.contexts, args.operations, log_path,
                )
            except ValueError as error:
                preserved = preserve_failed_log(log_path)
                print(
                    f"RETRY invalid_log={preserved} reason={error}",
                    flush=True,
                )
        if row is None:
            print(
                f"[{case_index}/{len(planned)}] latency={case['latency_us']}us "
                f"seed={case['seed']} mode={case['mode']}",
                flush=True,
            )
            with log_path.open("x") as stream:
                result = subprocess.run(
                    case["command"], cwd=REPO_ROOT, stdout=stream,
                    stderr=subprocess.STDOUT, text=True, check=False,
                )
            output = log_path.read_text(errors="replace")
            if result.returncode:
                raise RuntimeError(
                    f"case {case['run_id']} exited {result.returncode}; see {log_path}"
                )
            row = parse_case(
                output, case["mode"], case["latency_us"], case["seed"],
                args.contexts, args.operations, log_path,
            )
        rows.append(row)

    pairs = validate_pairs(rows)
    artifact_fingerprints = validate_campaign_artifacts(rows)
    aggregate = aggregate_rows(rows)
    pair_aggregate = aggregate_pairs(pairs)
    write_csv(output_dir / "results.csv", rows)
    write_csv(output_dir / "pairs.csv", pairs)
    write_csv(output_dir / "aggregate.csv", aggregate)
    write_csv(output_dir / "pair-aggregate.csv", pair_aggregate)
    write_json(
        output_dir / "results.json",
        {
            "schema": 1,
            "rows": rows,
            "pairs": pairs,
            "aggregate": aggregate,
            "pair_aggregate": pair_aggregate,
            "artifact_fingerprints": artifact_fingerprints,
        },
    )
    write_json(
        output_dir / "validation.json",
        {
            "schema": 1,
            "status": "pass",
            "completed": len(rows),
            "planned": len(planned),
            "paired_cases": len(pairs),
            "event_log": "off",
            "event_trace_callback": "off",
            "completion": "replay",
            "artifact_fingerprints": artifact_fingerprints,
        },
    )
    print(
        "OBMM_ASYNC_LOAD_SCHEDULER_COMPARE "
        f"cases={len(rows)} pairs={len(pairs)} event_log=off "
        "completion=replay status=pass"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, ValueError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
