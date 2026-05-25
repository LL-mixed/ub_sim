#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from pathlib import Path


PAIR_RE = re.compile(r"([A-Za-z0-9_]+)=([^ \r\n]+)")
RUN_SUMMARY_RE = re.compile(r"^eight_node_w5_inference_cluster_summary\.(.+)\.txt$")

BAD_MARKERS = (
    "status=missing",
    "qwen3_range_kv_state_lazy_fallback",
    "fallback=runtime_forward_metadata",
    "shortpath_ids=none",
    "support_ids=none",
    "artifact_kinds=none",
    "obmm_pool: unavailable",
)

ARTIFACT_LIMITS = {
    "memory_store_json": 16 * 1024 * 1024,
    "object_store_json": 8 * 1024 * 1024,
    "object_store_bin": 256 * 1024 * 1024,
    "shortpath_stream": 1024 * 1024,
    "shortpath_kv_stream": 1024 * 1024,
}


def parse_pairs(line):
    return dict(PAIR_RE.findall(line))


def parse_int(value, default=0):
    if value is None:
        return default
    try:
        return int(value, 0)
    except ValueError:
        return default


def split_count(value):
    if not value or "/" not in value:
        return (0, 0)
    left, right = value.split("/", 1)
    return (parse_int(left), parse_int(right))


def file_size(path):
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return None


def tree_size(path):
    if not path.exists():
        return None
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
    if value is None:
        return "missing"
    units = ("B", "KiB", "MiB", "GiB")
    current = float(value)
    for unit in units:
        if current < 1024 or unit == units[-1]:
            if unit == "B":
                return f"{int(current)}B"
            return f"{current:.1f}{unit}"
        current /= 1024
    return f"{value}B"


def infer_run_id(summary_path):
    match = RUN_SUMMARY_RE.match(summary_path.name)
    if not match:
        return ""
    return match.group(1)


def artifact_paths(summary_path, run_id, run_dir):
    out_dir = summary_path.parent
    registry_dir = out_dir / f"w5_memory_registry.{run_id}" if run_id else None
    paths = {
        "memory_store_json": out_dir / f"w5_memory_object_store.{run_id}.json",
        "object_store_json": out_dir / f"w5_object_service_store.{run_id}.json",
        "object_store_bin": out_dir / f"w5_object_service_store.{run_id}.bin",
    }
    if registry_dir is not None:
        paths["shortpath_stream"] = registry_dir / "w5_memory_shortpath_stream.txt"
        paths["shortpath_kv_stream"] = registry_dir / "w5_memory_shortpath_kv_stream.txt"
        paths["registry_dir"] = registry_dir
    if run_dir:
        paths["logs_dir"] = Path(run_dir)
    return paths


def parse_summary(summary_path):
    lines = summary_path.read_text(encoding="utf-8", errors="replace").splitlines()
    parsed = {
        "run_dir": "",
        "summary": {},
        "memory_service": {},
        "shortpath": {},
        "timing_steps": [],
        "timing_nodes": [],
        "engram_steps": [],
        "decode_output": [],
        "bad_markers": [],
    }

    for line in lines:
        if line.startswith("summary: run_dir="):
            parsed["run_dir"] = line.split("run_dir=", 1)[1].strip()
        elif line.startswith("summary: decode_steps_expected="):
            parsed["summary"] = parse_pairs(line)
        elif line.startswith("decode_output: "):
            parsed["decode_output"].append(line)
        elif line.startswith("memory_service_summary: "):
            parsed["memory_service"] = parse_pairs(line)
        elif line.startswith("guest_worker_shortpath_summary: "):
            parsed["shortpath"] = parse_pairs(line)
        elif line.startswith("timing_step: "):
            parsed["timing_steps"].append(parse_pairs(line))
        elif line.startswith("timing_node: "):
            parsed["timing_nodes"].append(parse_pairs(line))
        elif line.startswith("engram_timing_step: "):
            parsed["engram_steps"].append(parse_pairs(line))
        for marker in BAD_MARKERS:
            if marker in line:
                parsed["bad_markers"].append(marker)
    return parsed


def validate(parsed, paths):
    issues = []
    summary = parsed["summary"]
    shortpath = parsed["shortpath"]
    memory = parsed["memory_service"]

    expected = parse_int(summary.get("decode_steps_expected"))
    observed = parse_int(summary.get("decode_steps_observed"))
    passed_nodes = split_count(summary.get("passed_nodes", "0/0"))
    node_count = passed_nodes[1] or 8
    idle_expected = expected * max(node_count - 1, 0)

    if expected <= 0:
        issues.append("missing decode_steps_expected")
    if expected != observed:
        issues.append(f"decode steps mismatch expected={expected} observed={observed}")
    if passed_nodes[0] != passed_nodes[1] or passed_nodes[1] == 0:
        issues.append(f"passed_nodes incomplete value={summary.get('passed_nodes', '')}")
    if parse_int(summary.get("worker_timing_records")) != expected:
        issues.append("worker_timing_records does not match decode steps")
    if parse_int(summary.get("idle_timing_records")) != idle_expected:
        issues.append("idle_timing_records does not match shortpath idle expectation")

    if not memory:
        issues.append("missing memory_service_summary")
    else:
        memory_steps = split_count(memory.get("steps", "0/0"))
        if memory_steps != (expected, expected):
            issues.append(f"memory service step coverage mismatch value={memory.get('steps', '')}")
        if memory.get("actions") != "jump-to-terminal":
            issues.append(f"unexpected memory action value={memory.get('actions', '')}")
        if memory.get("artifact_kinds") != "logits":
            issues.append(f"unexpected artifact_kinds value={memory.get('artifact_kinds', '')}")
        if parse_int(memory.get("lookup_hits")) != expected:
            issues.append(f"lookup_hits mismatch value={memory.get('lookup_hits', '')}")

    if not shortpath:
        issues.append("missing guest_worker_shortpath_summary")
    else:
        expected_fields = {
            "boundary_hits": expected,
            "terminal_selects": expected,
            "expected_hits": expected,
            "actual_range_forwards": expected,
            "actual_runtime_inputs": max(expected - 1, 0),
            "actual_runtime_outputs": 0,
            "shortpath_no_dispatch": idle_expected,
            "shortpath_terminal_commits": idle_expected,
            "shortpath_publish_hidden_zero": expected,
            "full_pipeline_range_forwards": expected * node_count,
            "full_pipeline_runtime_inputs": max(expected * node_count - 1, 0),
            "full_pipeline_runtime_outputs": expected * node_count,
        }
        for key, value in expected_fields.items():
            actual = parse_int(shortpath.get(key))
            if actual != value:
                issues.append(f"{key} mismatch expected={value} actual={actual}")

    for marker in sorted(set(parsed["bad_markers"])):
        issues.append(f"bad marker present: {marker}")

    artifact_sizes = {}
    for label, limit in ARTIFACT_LIMITS.items():
        path = paths.get(label)
        size = file_size(path) if path is not None else None
        artifact_sizes[label] = {"path": str(path) if path else "", "bytes": size, "max_bytes": limit}
        if size is None:
            issues.append(f"missing artifact {label}")
        elif size > limit:
            issues.append(f"artifact {label} too large bytes={size} max_bytes={limit}")
    if paths.get("logs_dir"):
        artifact_sizes["logs_dir"] = {
            "path": str(paths["logs_dir"]),
            "bytes": tree_size(paths["logs_dir"]),
            "max_bytes": None,
        }
    if paths.get("registry_dir"):
        artifact_sizes["registry_dir"] = {
            "path": str(paths["registry_dir"]),
            "bytes": tree_size(paths["registry_dir"]),
            "max_bytes": None,
        }

    return issues, artifact_sizes


def timing_report(parsed):
    steps = parsed["timing_steps"]
    engram_steps = parsed["engram_steps"]
    round_sum = sum(parse_int(step.get("round_ms")) for step in steps)
    post_step0 = [
        parse_int(step.get("round_ms"))
        for step in steps
        if parse_int(step.get("step"), -1) > 0
    ]
    compute_sum = sum(parse_int(step.get("max_compute_window_ms")) for step in steps)
    publish_sum = sum(parse_int(step.get("max_publish_ms")) for step in steps)
    engram_sum = sum(parse_int(step.get("engram_total_ms")) for step in engram_steps)
    return {
        "steps": len(steps),
        "round_sum_ms": round_sum,
        "avg_round_ms": round(round_sum / len(steps), 1) if steps else 0.0,
        "post_step0_avg_round_ms": round(sum(post_step0) / len(post_step0), 1)
        if post_step0
        else 0.0,
        "compute_sum_ms": compute_sum,
        "publish_sum_ms": publish_sum,
        "engram_total_ms": engram_sum,
        "engram_avg_ms": round(engram_sum / len(engram_steps), 1) if engram_steps else 0.0,
    }


def build_report(summary_path):
    run_id = infer_run_id(summary_path)
    parsed = parse_summary(summary_path)
    paths = artifact_paths(summary_path, run_id, parsed["run_dir"])
    issues, artifact_sizes = validate(parsed, paths)
    summary = parsed["summary"]
    memory = parsed["memory_service"]
    shortpath = parsed["shortpath"]
    return {
        "summary_file": str(summary_path),
        "run_id": run_id,
        "status": "pass" if not issues else "fail",
        "issues": issues,
        "decode": {
            "steps_expected": parse_int(summary.get("decode_steps_expected")),
            "steps_observed": parse_int(summary.get("decode_steps_observed")),
            "passed_nodes": summary.get("passed_nodes", ""),
            "output": parsed["decode_output"],
        },
        "shortpath": {
            "lookup_hits": parse_int(memory.get("lookup_hits")),
            "actions": memory.get("actions", ""),
            "artifact_kinds": memory.get("artifact_kinds", ""),
            "boundary_hits": parse_int(shortpath.get("boundary_hits")),
            "actual_range_forwards": parse_int(shortpath.get("actual_range_forwards")),
            "actual_runtime_inputs": parse_int(shortpath.get("actual_runtime_inputs")),
            "actual_runtime_outputs": parse_int(shortpath.get("actual_runtime_outputs")),
            "shortpath_no_dispatch": parse_int(shortpath.get("shortpath_no_dispatch")),
            "shortpath_terminal_commits": parse_int(shortpath.get("shortpath_terminal_commits")),
        },
        "timing": timing_report(parsed),
        "artifacts": artifact_sizes,
    }


def print_text_report(report):
    print(
        "w5_run_report: "
        f"status={report['status']} run_id={report['run_id']} "
        f"summary={report['summary_file']}"
    )
    decode = report["decode"]
    print(
        "decode: "
        f"steps={decode['steps_observed']}/{decode['steps_expected']} "
        f"passed_nodes={decode['passed_nodes']}"
    )
    for line in decode["output"]:
        print(line)
    shortpath = report["shortpath"]
    print(
        "shortpath: "
        f"lookup_hits={shortpath['lookup_hits']} action={shortpath['actions']} "
        f"artifact_kinds={shortpath['artifact_kinds']} "
        f"boundary_hits={shortpath['boundary_hits']} "
        f"actual_range_forwards={shortpath['actual_range_forwards']} "
        f"actual_runtime_inputs={shortpath['actual_runtime_inputs']} "
        f"actual_runtime_outputs={shortpath['actual_runtime_outputs']} "
        f"shortpath_no_dispatch={shortpath['shortpath_no_dispatch']} "
        f"shortpath_terminal_commits={shortpath['shortpath_terminal_commits']}"
    )
    timing = report["timing"]
    print(
        "timing: "
        f"steps={timing['steps']} round_sum_ms={timing['round_sum_ms']} "
        f"avg_round_ms={timing['avg_round_ms']} "
        f"post_step0_avg_round_ms={timing['post_step0_avg_round_ms']} "
        f"compute_sum_ms={timing['compute_sum_ms']} "
        f"publish_sum_ms={timing['publish_sum_ms']} "
        f"engram_total_ms={timing['engram_total_ms']} "
        f"engram_avg_ms={timing['engram_avg_ms']}"
    )
    for label in sorted(report["artifacts"]):
        artifact = report["artifacts"][label]
        limit = artifact["max_bytes"]
        limit_text = format_bytes(limit) if limit is not None else "none"
        print(
            "artifact: "
            f"label={label} bytes={artifact['bytes']} "
            f"size={format_bytes(artifact['bytes'])} max={limit_text} "
            f"path={artifact['path']}"
        )
    if report["issues"]:
        for issue in report["issues"]:
            print(f"issue: {issue}")


def main(argv):
    parser = argparse.ArgumentParser(
        description="Audit a W5 inference cluster run summary and its artifacts."
    )
    parser.add_argument("summary", type=Path)
    parser.add_argument("--json", action="store_true", dest="json_output")
    args = parser.parse_args(argv)

    if not args.summary.is_file():
        print(f"summary file is missing: {args.summary}", file=sys.stderr)
        return 2

    report = build_report(args.summary)
    if args.json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_text_report(report)
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
