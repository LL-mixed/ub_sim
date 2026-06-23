#!/usr/bin/env python3
import argparse
import json
import re
import statistics
import sys
from pathlib import Path


PAIR_RE = re.compile(r"([A-Za-z0-9_]+)=([^ \r\n]+)")
DP_RESULT_RE = re.compile(r"\[obmm_dataplane_microbench\] result=done (?P<body>.*)$")
TCP_RESULT_RE = re.compile(r"\[ub_tcp_each_server\] benchmark_result=done (?P<body>.*)$")
STAT_RE = re.compile(r"([A-Za-z0-9_]+)=(-?(?:0x)?[0-9A-Fa-f]+)")


def parse_pairs(text):
    return dict(PAIR_RE.findall(text))


def parse_number(value, default=0.0):
    if value is None:
        return default
    try:
        if value.startswith(("0x", "-0x")):
            return float(int(value, 16))
        if "." in value:
            return float(value)
        return float(int(value, 10))
    except ValueError:
        return default


def parse_report(path):
    pairs = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        pairs[key.strip()] = value.strip()
    return pairs


def sample_from_pairs(case, node, pairs, source):
    duration_ms = parse_number(pairs.get("duration_ms"))
    read_mbps = parse_number(pairs.get("read_MBps") or pairs.get("read_mbps"))
    write_mbps = parse_number(pairs.get("write_MBps") or pairs.get("write_mbps"))
    return {
        "case": case,
        "node": node,
        "duration_ms": duration_ms,
        "read_MBps": read_mbps,
        "write_MBps": write_mbps,
        "read_bytes": int(parse_number(pairs.get("read_bytes"))),
        "write_bytes": int(parse_number(pairs.get("write_bytes"))),
        "verify_failures": int(parse_number(pairs.get("verify_failures"))),
        "source": str(source),
    }


def node_from_log(path):
    name = path.name
    if name.endswith("_guest.log"):
        return name[: -len("_guest.log")]
    return path.stem


def parse_guest_log(path):
    samples = []
    node = node_from_log(path)
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except FileNotFoundError:
        return samples
    for line in lines:
        match = DP_RESULT_RE.search(line)
        if match:
            pairs = parse_pairs(match.group("body"))
            samples.append(sample_from_pairs(pairs.get("mode", "unknown"), node, pairs, path))
            continue
        match = TCP_RESULT_RE.search(line)
        if match:
            pairs = parse_pairs(match.group("body"))
            samples.append(sample_from_pairs("tcp", node, pairs, path))
    return samples


def parse_qemu_stats(path):
    totals = {}
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except FileNotFoundError:
        return totals
    for line in lines:
        if "GVA_STATS " not in line and "SIM_DEC_STATS " not in line:
            continue
        prefix = "gva" if "GVA_STATS " in line else "sim_dec"
        for key, value in STAT_RE.findall(line):
            totals[f"{prefix}_{key}"] = totals.get(f"{prefix}_{key}", 0) + int(value, 0)
    return totals


def merge_stats(left, right):
    merged = dict(left)
    for key, value in right.items():
        merged[key] = merged.get(key, 0) + value
    return merged


def collect_run(report_path):
    report = parse_report(report_path)
    run_dir = Path(report.get("run_dir", ""))
    samples = []
    qemu_stats = {}
    if run_dir.exists():
        for guest_log in sorted(run_dir.glob("*_guest.log")):
            samples.extend(parse_guest_log(guest_log))
        for qemu_log in sorted(run_dir.glob("*_qemu.log")):
            qemu_stats = merge_stats(qemu_stats, parse_qemu_stats(qemu_log))
    return {
        "report": str(report_path),
        "run_id": report.get("run_id", ""),
        "result": report.get("result", ""),
        "run_dir": str(run_dir),
        "samples": samples,
        "qemu_stats": qemu_stats,
    }


def summarize_case(samples):
    durations = [sample["duration_ms"] for sample in samples]
    read_rates = [sample["read_MBps"] for sample in samples]
    write_rates = [sample["write_MBps"] for sample in samples]
    read_bytes = sum(sample["read_bytes"] for sample in samples)
    write_bytes = sum(sample["write_bytes"] for sample in samples)
    verify_failures = sum(sample["verify_failures"] for sample in samples)
    return {
        "samples": len(samples),
        "duration_ms_avg": statistics.fmean(durations) if durations else 0.0,
        "duration_ms_median": statistics.median(durations) if durations else 0.0,
        "duration_ms_min": min(durations) if durations else 0.0,
        "duration_ms_max": max(durations) if durations else 0.0,
        "read_MBps_avg": statistics.fmean(read_rates) if read_rates else 0.0,
        "write_MBps_avg": statistics.fmean(write_rates) if write_rates else 0.0,
        "read_bytes": read_bytes,
        "write_bytes": write_bytes,
        "verify_failures": verify_failures,
    }


def summarize_runs(runs):
    by_case = {}
    for run in runs:
        for sample in run["samples"]:
            by_case.setdefault(sample["case"], []).append(sample)
    cases = {case: summarize_case(samples) for case, samples in sorted(by_case.items())}
    return {
        "runs": runs,
        "cases": cases,
        "deltas": build_deltas(cases),
    }


def ratio(num, denom):
    if denom == 0:
        return 0.0
    return num / denom


def build_deltas(cases):
    deltas = {}
    baseline = cases.get("legacy-pa")
    if not baseline:
        return deltas
    base_duration = baseline["duration_ms_median"]
    base_write = baseline["write_MBps_avg"]
    for case in ("generic-gva", "gsva"):
        summary = cases.get(case)
        if not summary:
            continue
        deltas[case] = {
            "duration_speedup_vs_legacy_pa": ratio(base_duration, summary["duration_ms_median"]),
            "write_MBps_speedup_vs_legacy_pa": ratio(summary["write_MBps_avg"], base_write),
        }
    return deltas


def print_text(summary, missing):
    if missing:
        print(f"transport_perf_report: status=fail reason=no_samples reports={','.join(missing)}")
    else:
        print("transport_perf_report: status=pass")
    for run in summary["runs"]:
        print(
            "transport_run: "
            f"run_id={run['run_id']} result={run['result']} "
            f"samples={len(run['samples'])} report={run['report']}"
        )
    for case, values in summary["cases"].items():
        print(
            "transport_case: "
            f"name={case} samples={values['samples']} "
            f"duration_ms_median={values['duration_ms_median']:.3f} "
            f"duration_ms_avg={values['duration_ms_avg']:.3f} "
            f"duration_ms_min={values['duration_ms_min']:.3f} "
            f"duration_ms_max={values['duration_ms_max']:.3f} "
            f"read_MBps_avg={values['read_MBps_avg']:.3f} "
            f"write_MBps_avg={values['write_MBps_avg']:.3f} "
            f"verify_failures={values['verify_failures']}"
        )
    for case, values in summary["deltas"].items():
        print(
            "transport_delta: "
            f"case={case} baseline=legacy-pa "
            f"duration_speedup={values['duration_speedup_vs_legacy_pa']:.3f} "
            f"write_MBps_speedup={values['write_MBps_speedup_vs_legacy_pa']:.3f}"
        )
    merged_stats = {}
    for run in summary["runs"]:
        merged_stats = merge_stats(merged_stats, run["qemu_stats"])
    if merged_stats:
        stat_text = " ".join(f"{key}={value}" for key, value in sorted(merged_stats.items()))
        print(f"transport_qemu_stats: {stat_text}")


def main(argv):
    parser = argparse.ArgumentParser(description="Summarize UB/GVA/GSVA/TCP transport benchmark logs.")
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--json", action="store_true", dest="json_output")
    args = parser.parse_args(argv)

    runs = [collect_run(path) for path in args.reports]
    summary = summarize_runs(runs)
    missing = [str(run["report"]) for run in runs if not run["samples"]]
    if args.json_output:
        print(json.dumps(summary, indent=2, sort_keys=True))
    else:
        print_text(summary, missing)
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
