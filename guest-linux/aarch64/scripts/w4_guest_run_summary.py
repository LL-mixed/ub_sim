#!/usr/bin/env python3
import collections
import json
import os
import re
import sys


PAIR_RE = re.compile(r"([A-Za-z0-9_]+)=([^ \r\n]+)")


def parse_pairs(line):
    return dict(PAIR_RE.findall(line))


def parse_int(value, default=0):
    if value is None:
        return default
    try:
        return int(value, 0)
    except ValueError:
        return default


def decode_piece(fields):
    word0 = parse_int(fields.get("piece_word0"), 0)
    word1 = parse_int(fields.get("piece_word1"), 0)
    data = word0.to_bytes(8, "little") + word1.to_bytes(8, "little")
    data = data.rstrip(b"\x00")
    return data.decode("utf-8", errors="replace")


def display_piece(piece):
    return piece.replace("Ġ", " ")


def quote_text(value):
    return json.dumps(value, ensure_ascii=False)


def shorten(value, limit=220):
    if len(value) <= limit:
        return value
    return value[: limit - 3] + "..."


def format_duration(seconds):
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    if hours:
        return f"{hours:02d}:{minutes:02d}:{seconds:02d}"
    return f"{minutes:02d}:{seconds:02d}"


def progress_bar(done, total, width=24):
    if total <= 0:
        filled = 0
    else:
        filled = round((done / total) * width)
    filled = max(0, min(width, filled))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def compact_node_name(node_id):
    if node_id.startswith("node") and len(node_id) > 4:
        return node_id[4:]
    return node_id


def parse_run_logs(run_dir, expected_steps, node_ids):
    tokens = {}
    timings = []
    handoff_timings = []
    barriers = {}
    pool_usage = {}
    passes = {node_id: 0 for node_id in node_ids}
    missing_logs = []
    latest_status = {}

    for node_id in node_ids:
        log_path = os.path.join(run_dir, f"{node_id}_guest.log")
        if not os.path.exists(log_path):
            missing_logs.append(log_path)
            continue

        with open(log_path, "r", encoding="utf-8", errors="replace") as log_file:
            for raw_line in log_file:
                clean_line = raw_line.rstrip("\n").rstrip("\r")
                if clean_line == "[w4_guest] pass":
                    passes[node_id] += 1
                if clean_line.startswith("[w4_guest] stage ") or clean_line.startswith(
                    "[w4_guest] step="
                ):
                    latest_status[node_id] = clean_line[len("[w4_guest] ") :]

                if "qwen3_terminal_token_result_publish" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        fields["_log_node"] = node_id
                        fields["_piece"] = decode_piece(fields)
                        fields["_display_piece"] = display_piece(fields["_piece"])
                        tokens[step] = fields

                if "qwen3_worker_timing" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        record = {
                            "_log_node": node_id,
                            "step": step,
                            "local": fields.get("local", node_id),
                            "layers": fields.get("layers", ""),
                        }
                        for key in (
                            "node",
                            "count",
                            "next",
                            "total_ms",
                            "terminal_gate_ms",
                            "setup_ms",
                            "obmm_stage_ms",
                            "cluster_ms",
                            "map_ms",
                            "seed_payload_ms",
                            "descriptor_ms",
                            "input_wait_ms",
                            "compute_window_ms",
                            "submit_ms",
                            "base_submit_ms",
                            "doorbell_submit_ms",
                            "max_batch_submit_ms",
                            "dispatch_ms",
                            "doorbell_log_ms",
                            "batch_sleep_ms",
                            "post_batch_ms",
                            "completion_decode_ms",
                            "publish_ms",
                            "verify_publish_ms",
                            "round_done_ms",
                            "barrier_ms",
                            "unaccounted_ms",
                        ):
                            record[key] = parse_int(fields.get(key), 0)
                        timings.append(record)

                if "qwen3_worker_handoff_timing" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        record = {
                            "_log_node": node_id,
                            "step": step,
                            "local": fields.get("local", node_id),
                            "layers": fields.get("layers", ""),
                        }
                        for key in (
                            "node",
                            "source",
                            "next",
                            "clock_offset_ms",
                            "input_found_supernode_ms",
                            "handoff_publish_supernode_ms",
                            "publish_done_supernode_ms",
                            "producer_publish_supernode_ms",
                            "producer_publish_mono_ms",
                            "producer_clock_offset_ms",
                            "producer_to_input_found_supernode_ms",
                            "producer_to_input_found_mono_ms",
                            "input_wait_ms",
                            "input_activate_ms",
                            "input_metadata_ms",
                            "input_wait_attempts",
                            "input_found_to_handoff_ms",
                            "input_loaded_to_handoff_ms",
                            "kv_resolve_ms",
                            "kv_load_ms",
                            "compute_window_ms",
                            "submit_ms",
                            "dispatch_ms",
                            "completion_decode_ms",
                            "verify_dispatch_ms",
                            "range_publish_ms",
                            "terminal_publish_ms",
                            "compute_done_to_handoff_ms",
                            "round_done_publish_ms",
                        ):
                            record[key] = parse_int(fields.get(key), 0)
                        handoff_timings.append(record)

                if "qwen3_worker_barrier_timing" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        barriers[(node_id, step)] = {
                            "barrier_ms": parse_int(fields.get("barrier_ms"), 0),
                            "total_with_barrier_ms": parse_int(fields.get("total_with_barrier_ms"), 0),
                        }

                if "qwen3_obmm_pool_usage" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        record = {"_log_node": node_id, "step": step}
                        for key in (
                            "per_node_region_bytes",
                            "cluster_region_bytes",
                            "payload_bytes",
                            "payload_high_water_bytes",
                            "payload_used_pct_milli",
                            "arena_base",
                            "arena_used_bytes",
                            "arena_next",
                        ):
                            record[key] = parse_int(fields.get(key), 0)
                        pool_usage[node_id] = record

    return tokens, timings, handoff_timings, barriers, pool_usage, passes, missing_logs, latest_status


def node_round_ms(record, barriers):
    barrier = barriers.get((record["_log_node"], record["step"]))
    if barrier and barrier["total_with_barrier_ms"] > 0:
        return barrier["total_with_barrier_ms"]
    return record["total_ms"]


def emit_summary(run_dir, expected_steps, node_ids, output):
    tokens, timings, handoff_timings, barriers, pool_usage, passes, missing_logs, _latest_status = parse_run_logs(
        run_dir, expected_steps, node_ids
    )
    passed_nodes = sum(1 for count in passes.values() if count >= expected_steps)

    output.append(f"summary: run_dir={run_dir}")
    output.append(
        "summary: "
        f"decode_steps_expected={expected_steps} "
        f"decode_steps_observed={len(tokens)} "
        f"worker_timing_records={len(timings)} "
        f"passed_nodes={passed_nodes}/{len(node_ids)} "
        f"handoff_timing_records={len(handoff_timings)}"
    )
    if missing_logs:
        output.append(f"summary: missing_guest_logs={quote_text(missing_logs)}")

    emit_token_summary(tokens, expected_steps, output)
    emit_timing_summary(timings, barriers, expected_steps, node_ids, output)
    emit_handoff_timing_summary(handoff_timings, expected_steps, node_ids, output)
    emit_pool_usage_summary(pool_usage, expected_steps, node_ids, output)


def emit_progress(run_dir, expected_steps, elapsed_s, node_ids, output):
    tokens, _timings, _handoff_timings, _barriers, _pool_usage, passes, missing_logs, latest_status = parse_run_logs(
        run_dir, expected_steps, node_ids
    )
    pad = max(1, len(str(expected_steps)))
    count_parts = [
        f"{compact_node_name(node_id)}={passes.get(node_id, 0):0{pad}d}/{expected_steps}"
        for node_id in node_ids
    ]
    min_passes = min((passes.get(node_id, 0) for node_id in node_ids), default=0)
    max_passes = max((passes.get(node_id, 0) for node_id in node_ids), default=0)
    percent = 0 if expected_steps <= 0 else round((min_passes / expected_steps) * 100)
    slowest_node = next(
        (node_id for node_id in node_ids if passes.get(node_id, 0) == min_passes),
        "unknown",
    )

    latest_token = "none"
    terminal_tokens = len(tokens)
    if tokens:
        latest_step = max(tokens)
        fields = tokens[latest_step]
        latest_token = (
            f"step={latest_step} "
            f"token={fields.get('token', '0')} "
            f"piece={quote_text(fields.get('_display_piece', ''))} "
            f"runner_up={fields.get('runner_up', '0')} "
            f"margin_milli={fields.get('margin_milli', '0')}"
        )

    output.append(
        "progress: "
        f"elapsed={format_duration(elapsed_s)} "
        f"cluster_decode={min_passes}/{expected_steps} "
        f"({percent}%) "
        f"terminal_tokens={terminal_tokens}/{expected_steps} "
        f"latest_token={latest_token}"
    )
    output.append(
        "progress: "
        f"cluster_bar={progress_bar(min_passes, expected_steps)} "
        f"node_range={min_passes}..{max_passes}/{expected_steps} "
        f"lagging={slowest_node}"
    )
    output.append(f"progress: node_passes {' '.join(count_parts)}")
    latest = shorten(latest_status.get(slowest_node, "unknown"))
    output.append(
        "progress: "
        f"lagging_status node={slowest_node} "
        f"passes={min_passes}/{expected_steps} "
        f"latest={quote_text(latest)}"
    )
    if missing_logs:
        output.append(f"progress: missing_guest_logs={quote_text(missing_logs)}")


def emit_token_summary(tokens, expected_steps, output):
    if not tokens:
        output.append("decode_output: unavailable reason=no_qwen3_terminal_token_result_publish")
        return

    ordered_steps = sorted(tokens)
    token_ids = [parse_int(tokens[step].get("token"), 0) for step in ordered_steps]
    token_pieces = [tokens[step].get("_display_piece", "") for step in ordered_steps]
    missing_steps = [step for step in range(expected_steps) if step not in tokens]
    output.append(f"decode_output: token_ids={json.dumps(token_ids)}")
    output.append(f"decode_output: token_pieces={quote_text(''.join(token_pieces))}")
    if missing_steps:
        output.append(f"decode_output: missing_steps={json.dumps(missing_steps)}")
    for step in ordered_steps:
        fields = tokens[step]
        output.append(
            "decode_token: "
            f"step={step} "
            f"node={fields.get('_log_node', '')} "
            f"token={fields.get('token', '0')} "
            f"piece={quote_text(fields.get('_display_piece', ''))} "
            f"runner_up={fields.get('runner_up', '0')} "
            f"margin_milli={fields.get('margin_milli', '0')} "
            f"text_checksum={fields.get('text_checksum', '0x0')}"
        )


def emit_timing_summary(timings, barriers, expected_steps, node_ids, output):
    if not timings:
        output.append("timing: unavailable reason=no_qwen3_worker_timing_records")
        return

    timings_by_step = collections.defaultdict(list)
    timings_by_node = collections.defaultdict(list)
    for record in timings:
        timings_by_step[record["step"]].append(record)
        timings_by_node[record["_log_node"]].append(record)

    step_summaries = []
    for step in sorted(timings_by_step):
        records = timings_by_step[step]
        critical = max(records, key=lambda record: node_round_ms(record, barriers))
        max_worker_ms = max(record["total_ms"] for record in records)
        avg_worker_ms = sum(record["total_ms"] for record in records) // len(records)
        max_input_wait_ms = max(record["input_wait_ms"] for record in records)
        max_compute_window_ms = max(record["compute_window_ms"] for record in records)
        max_submit_ms = max(record["submit_ms"] for record in records)
        max_publish_ms = max(record["publish_ms"] for record in records)
        max_barrier_ms = max(
            barriers.get((record["_log_node"], step), {}).get("barrier_ms", 0)
            for record in records
        )
        summary = {
            "step": step,
            "round_ms": node_round_ms(critical, barriers),
            "critical_node": critical["_log_node"],
        }
        step_summaries.append(summary)
        output.append(
            "timing_step: "
            f"step={step} "
            f"round_ms={summary['round_ms']} "
            f"critical_node={summary['critical_node']} "
            f"workers={len(records)}/{len(node_ids)} "
            f"max_worker_ms={max_worker_ms} "
            f"avg_worker_ms={avg_worker_ms} "
            f"max_input_wait_ms={max_input_wait_ms} "
            f"max_compute_window_ms={max_compute_window_ms} "
            f"max_submit_ms={max_submit_ms} "
            f"max_publish_ms={max_publish_ms} "
            f"max_barrier_ms={max_barrier_ms}"
        )

    for node_id in node_ids:
        records = sorted(timings_by_node.get(node_id, []), key=lambda item: item["step"])
        if not records:
            output.append(f"timing_node: node={node_id} steps=0/{expected_steps} status=missing")
            continue
        worker_total_ms = sum(record["total_ms"] for record in records)
        wall_total_ms = sum(node_round_ms(record, barriers) for record in records)
        barrier_total_ms = sum(
            barriers.get((node_id, record["step"]), {}).get("barrier_ms", 0)
            for record in records
        )
        max_worker_ms = max(record["total_ms"] for record in records)
        avg_worker_ms = worker_total_ms // len(records)
        output.append(
            "timing_node: "
            f"node={node_id} "
            f"steps={len(records)}/{expected_steps} "
            f"worker_total_ms={worker_total_ms} "
            f"wall_total_ms={wall_total_ms} "
            f"barrier_total_ms={barrier_total_ms} "
            f"max_worker_ms={max_worker_ms} "
            f"avg_worker_ms={avg_worker_ms}"
        )

    slowest_step = max(step_summaries, key=lambda item: item["round_ms"])
    max_input_record = max(timings, key=lambda item: item["input_wait_ms"])
    max_compute_record = max(timings, key=lambda item: item["compute_window_ms"])
    output.append(
        "timing_bottleneck: "
        f"slowest_step={slowest_step['step']} "
        f"round_ms={slowest_step['round_ms']} "
        f"critical_node={slowest_step['critical_node']}"
    )
    output.append(
        "timing_bottleneck: "
        f"max_input_wait_step={max_input_record['step']} "
        f"node={max_input_record['_log_node']} "
        f"input_wait_ms={max_input_record['input_wait_ms']} "
        f"worker_total_ms={max_input_record['total_ms']}"
    )
    output.append(
        "timing_bottleneck: "
        f"max_compute_step={max_compute_record['step']} "
        f"node={max_compute_record['_log_node']} "
        f"compute_window_ms={max_compute_record['compute_window_ms']} "
        f"worker_total_ms={max_compute_record['total_ms']}"
    )


def emit_handoff_timing_summary(handoff_timings, expected_steps, node_ids, output):
    if not handoff_timings:
        output.append("handoff_timing: unavailable reason=no_qwen3_worker_handoff_timing_records")
        return

    handoffs_by_step = collections.defaultdict(list)
    handoffs_by_node = collections.defaultdict(list)
    for record in handoff_timings:
        handoffs_by_step[record["step"]].append(record)
        handoffs_by_node[record["_log_node"]].append(record)

    for step in sorted(handoffs_by_step):
        records = handoffs_by_step[step]
        max_handoff = max(records, key=lambda item: item["input_found_to_handoff_ms"])
        max_dispatch = max(records, key=lambda item: item["dispatch_ms"])
        max_publish = max(records, key=lambda item: item["range_publish_ms"])
        output.append(
            "handoff_step: "
            f"step={step} "
            f"workers={len(records)}/{len(node_ids)} "
            f"critical_node={max_handoff['_log_node']} "
            f"input_found_to_handoff_ms={max_handoff['input_found_to_handoff_ms']} "
            f"input_loaded_to_handoff_ms={max_handoff['input_loaded_to_handoff_ms']} "
            f"dispatch_node={max_dispatch['_log_node']} "
            f"dispatch_ms={max_dispatch['dispatch_ms']} "
            f"publish_node={max_publish['_log_node']} "
            f"range_publish_ms={max_publish['range_publish_ms']} "
            f"producer_to_input_found_mono_ms={max_handoff['producer_to_input_found_mono_ms']} "
            f"producer_to_input_found_supernode_ms={max_handoff['producer_to_input_found_supernode_ms']}"
        )

    max_handoff_record = max(
        handoff_timings,
        key=lambda item: item["input_found_to_handoff_ms"],
    )
    max_kv_record = max(handoff_timings, key=lambda item: item["kv_resolve_ms"] + item["kv_load_ms"])
    max_publish_record = max(handoff_timings, key=lambda item: item["range_publish_ms"])
    output.append(
        "handoff_bottleneck: "
        f"max_handoff_step={max_handoff_record['step']} "
        f"node={max_handoff_record['_log_node']} "
        f"input_found_to_handoff_ms={max_handoff_record['input_found_to_handoff_ms']} "
        f"compute_window_ms={max_handoff_record['compute_window_ms']} "
        f"dispatch_ms={max_handoff_record['dispatch_ms']} "
        f"range_publish_ms={max_handoff_record['range_publish_ms']}"
    )
    output.append(
        "handoff_bottleneck: "
        f"max_kv_step={max_kv_record['step']} "
        f"node={max_kv_record['_log_node']} "
        f"kv_resolve_ms={max_kv_record['kv_resolve_ms']} "
        f"kv_load_ms={max_kv_record['kv_load_ms']}"
    )
    output.append(
        "handoff_bottleneck: "
        f"max_publish_step={max_publish_record['step']} "
        f"node={max_publish_record['_log_node']} "
        f"range_publish_ms={max_publish_record['range_publish_ms']} "
        f"terminal_publish_ms={max_publish_record['terminal_publish_ms']}"
    )

    for node_id in node_ids:
        records = sorted(handoffs_by_node.get(node_id, []), key=lambda item: item["step"])
        if not records:
            output.append(f"handoff_node: node={node_id} steps=0/{expected_steps} status=missing")
            continue
        total_handoff_ms = sum(record["input_found_to_handoff_ms"] for record in records)
        total_dispatch_ms = sum(record["dispatch_ms"] for record in records)
        total_publish_ms = sum(record["range_publish_ms"] for record in records)
        output.append(
            "handoff_node: "
            f"node={node_id} "
            f"steps={len(records)}/{expected_steps} "
            f"total_input_found_to_handoff_ms={total_handoff_ms} "
            f"total_dispatch_ms={total_dispatch_ms} "
            f"total_range_publish_ms={total_publish_ms} "
            f"max_input_found_to_handoff_ms={max(record['input_found_to_handoff_ms'] for record in records)}"
        )


def emit_pool_usage_summary(pool_usage, expected_steps, node_ids, output):
    if not pool_usage:
        output.append("obmm_pool: unavailable reason=no_qwen3_obmm_pool_usage_records")
        return

    observed = [pool_usage[node_id] for node_id in node_ids if node_id in pool_usage]
    max_payload = max(record["payload_high_water_bytes"] for record in observed)
    max_arena = max(record["arena_used_bytes"] for record in observed)
    max_pct = max(record["payload_used_pct_milli"] for record in observed)
    per_node_region = max(record["per_node_region_bytes"] for record in observed)
    cluster_region = max(record["cluster_region_bytes"] for record in observed)
    output.append(
        "obmm_pool: "
        f"nodes_observed={len(observed)}/{len(node_ids)} "
        f"expected_steps={expected_steps} "
        f"per_node_region_bytes={per_node_region} "
        f"cluster_region_bytes={cluster_region} "
        f"max_payload_high_water_bytes={max_payload} "
        f"max_arena_used_bytes={max_arena} "
        f"max_payload_used_pct_milli={max_pct}"
    )
    for node_id in node_ids:
        record = pool_usage.get(node_id)
        if record is None:
            output.append(f"obmm_pool_node: node={node_id} status=missing")
            continue
        output.append(
            "obmm_pool_node: "
            f"node={node_id} "
            f"step={record['step']} "
            f"payload_bytes={record['payload_bytes']} "
            f"payload_high_water_bytes={record['payload_high_water_bytes']} "
            f"payload_used_pct_milli={record['payload_used_pct_milli']} "
            f"arena_used_bytes={record['arena_used_bytes']}"
        )


def main(argv):
    if len(argv) >= 2 and argv[1] == "--progress":
        if len(argv) < 6:
            print(
                "usage: w4_guest_run_summary.py --progress RUN_DIR EXPECTED_STEPS "
                "ELAPSED_SECS NODE_ID...",
                file=sys.stderr,
            )
            return 2

        run_dir = argv[2]
        expected_steps = int(argv[3])
        elapsed_s = int(argv[4])
        node_ids = argv[5:]
        output = []
        emit_progress(run_dir, expected_steps, elapsed_s, node_ids, output)
        for line in output:
            print(line)
        return 0

    if len(argv) < 4:
        print(
            "usage: w4_guest_run_summary.py RUN_DIR EXPECTED_STEPS NODE_ID...",
            file=sys.stderr,
        )
        return 2

    run_dir = argv[1]
    expected_steps = int(argv[2])
    node_ids = argv[3:]
    output = []
    emit_summary(run_dir, expected_steps, node_ids, output)
    for line in output:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
