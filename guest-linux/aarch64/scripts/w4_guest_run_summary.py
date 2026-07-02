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


def parse_layers(value):
    if not value:
        return (0, 0)
    match = re.fullmatch(r"\[([0-9]+),([0-9]+)\)", value)
    if not match:
        return (0, 0)
    return (int(match.group(1)), int(match.group(2)))


def shorten(value, limit=220):
    if len(value) <= limit:
        return value
    return value[: limit - 3] + "..."


def csv_or_none(values):
    ordered = sorted({value for value in values if value and value != "none"})
    return ",".join(ordered) if ordered else "none"


def csv_or_none_ordered(values):
    seen = set()
    ordered = []
    for value in values:
        if not value or value == "none" or value in seen:
            continue
        seen.add(value)
        ordered.append(value)
    return ",".join(ordered) if ordered else "none"


def lookup_hit_registry_step(record):
    value = record.get("registry_step")
    if value and value != "none":
        return value
    step = record.get("step", -1)
    return str(step) if step >= 0 else "none"


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
    idle_timings = []
    engram_timings = []
    engram_context_records = []
    memory_records = []
    device_records = []
    boundary_observations = []
    worker_events = collections.Counter()
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
                if clean_line.startswith("[mem_service] stage "):
                    latest_status[node_id] = clean_line[len("[mem_service] ") :]

                if "qwen3_terminal_token_result_publish" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        fields["_log_node"] = node_id
                        fields["_piece"] = decode_piece(fields)
                        fields["_display_piece"] = display_piece(fields["_piece"])
                        tokens[step] = fields

                if (
                    "qwen3_w5_memory_" in clean_line
                    or "qwen3_memory_service_boundary_lookup_" in clean_line
                ):
                    fields = parse_pairs(clean_line)
                    stage = ""
                    if "stage " in clean_line:
                        stage = clean_line.split("stage ", 1)[1].split(" ", 1)[0]
                    record = dict(fields)
                    record["_log_node"] = node_id
                    record["stage"] = stage
                    record["step"] = parse_int(fields.get("step"), -1)
                    memory_records.append(record)

                if "stage qwen3_w5_device_" in clean_line:
                    fields = parse_pairs(clean_line)
                    stage = clean_line.split("stage ", 1)[1].split(" ", 1)[0]
                    record = dict(fields)
                    record["_log_node"] = node_id
                    record["stage"] = stage
                    record["step"] = parse_int(fields.get("step"), -1)
                    device_records.append(record)

                if "stage uapi_qwen3_range_runtime_forward " in clean_line:
                    worker_events["range_forwards"] += 1
                if "stage qwen3_range_forward_runtime_input_loaded " in clean_line:
                    worker_events["runtime_inputs"] += 1
                if "stage qwen3_range_forward_runtime_output_publish " in clean_line:
                    worker_events["runtime_outputs"] += 1
                if "stage qwen3_decode_round_scheduler_no_dispatch " in clean_line:
                    worker_events["shortpath_no_dispatches"] += 1
                if "stage qwen3_decode_round_terminal_committed " in clean_line:
                    worker_events["shortpath_terminal_commits"] += 1
                if (
                    "stage qwen3_w5_memory_shortpath_commit " in clean_line
                    and " publish_hidden=0 " in clean_line
                ):
                    worker_events["shortpath_publish_hidden_zero"] += 1

                if "qwen3_range_forward_runtime_ingress_publish" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        layer_start, layer_end = parse_layers(fields.get("layers"))
                        hidden_bytes = parse_int(fields.get("bytes"), 0)
                        if hidden_bytes > 0 and hidden_bytes % 4 == 0:
                            hidden_dtype = "F32"
                            hidden_shape = str(hidden_bytes // 4)
                        else:
                            hidden_dtype = "Opaque"
                            hidden_shape = str(hidden_bytes)
                        boundary_observations.append(
                            {
                                "_log_node": node_id,
                                "step": step,
                                "observation_id": fields.get("observation_id", ""),
                                "node": fields.get("local", node_id),
                                "target": fields.get("target", ""),
                                "layers": fields.get("layers", ""),
                                "layer_start": layer_start,
                                "layer_end": layer_end,
                                "layer_count": parse_int(fields.get("count"), 0),
                                "hidden_key": fields.get("key", ""),
                                "hidden_key_hash": fields.get("key_hash", "0x0"),
                                "hidden_version": parse_int(fields.get("version"), 0),
                                "hidden_checksum": fields.get("checksum", "0x0"),
                                "hidden_bytes": hidden_bytes,
                                "hidden_dtype": hidden_dtype,
                                "hidden_shape": hidden_shape,
                                "producer_publish_ms": parse_int(
                                    fields.get("producer_publish_ms"), 0
                                ),
                                "producer_publish_mono_ms": parse_int(
                                    fields.get("producer_publish_mono_ms"), 0
                                ),
                                "backing": fields.get("backing", ""),
                                "metadata": fields.get("metadata", ""),
                                "queue": fields.get("queue", ""),
                                "status": fields.get("status", ""),
                            }
                        )

                if "qwen3_worker_timing" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        record = {
                            "_log_node": node_id,
                            "step": step,
                            "local": fields.get("local", node_id),
                            "layers": fields.get("layers", ""),
                            "kv_backend": fields.get("kv_backend", ""),
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

                if "qwen3_decode_round_idle_timing" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        idle_timings.append(
                            {
                                "_log_node": node_id,
                                "step": step,
                                "local": fields.get("local", node_id),
                                "node": parse_int(fields.get("node"), 0),
                                "terminal_observed": parse_int(
                                    fields.get("terminal_observed"), 0
                                ),
                                "input_wait_ms": parse_int(fields.get("input_wait_ms"), 0),
                                "round_done_ms": parse_int(fields.get("round_done_ms"), 0),
                                "source": fields.get("source", ""),
                                "status": fields.get("status", ""),
                            }
                        )

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
                            "gsva_lookup_ms",
                            "gsva_map_read_ms",
                            "prefix_cache_avoided_compute_ms",
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

                if "qwen3_engram_timing" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        record = {
                            "_log_node": node_id,
                            "step": step,
                            "local": fields.get("local", node_id),
                            "owner": fields.get("owner", ""),
                            "status": fields.get("status", ""),
                            "work_item": fields.get("work_item", "range_forward"),
                        }
                        for key in (
                            "node",
                            "candidate_publish_ms",
                            "candidate_wait_ms",
                            "policy_select_ms",
                            "decision_publish_ms",
                            "selected_wait_ms",
                            "selected_writeback_ms",
                            "history_state_wait_ms",
                            "qwen3_range_publish_ms",
                            "qwen3_range_input_wait_ms",
                        ):
                            record[key] = parse_int(fields.get(key), 0)
                        engram_timings.append(record)

                if "qwen3_engram_context_object_refs_loaded" in clean_line:
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), None)
                    if step is not None:
                        state_checksum = fields.get("state_checksum", "0x0")
                        engram_context_records.append(
                            {
                                "_log_node": node_id,
                                "step": step,
                                "mode": "object-ref",
                                "output_checksum": state_checksum,
                                "gate_checksum": "0x0",
                                "index_checksum": "0x0",
                                "table_rows": parse_int(fields.get("refs"), 0),
                                "output_l1_milli": 0,
                                "latency_ms": 0,
                                "row_prefetch_hits": 0,
                                "row_prefetch_requests": 0,
                                "row_prefetch_hit_rate_milli": 0,
                                "table_bytes_moved": 0,
                                "gate_weight_bytes_moved": 0,
                                "indices_bytes_moved": 0,
                                "hidden_input_bytes": 0,
                                "hidden_output_bytes": 0,
                                "hidden_injection_overhead_bytes": 0,
                            }
                        )

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

        qemu_log_path = os.path.join(run_dir, f"{node_id}_qemu.log")
        if os.path.exists(qemu_log_path):
            context_step = 0
            with open(qemu_log_path, "r", encoding="utf-8", errors="replace") as qemu_log_file:
                for raw_line in qemu_log_file:
                    clean_line = raw_line.rstrip("\n").rstrip("\r")
                    if "qwen3_w5_memory_" in clean_line:
                        fields = parse_pairs(clean_line)
                        stage = ""
                        if "stage " in clean_line:
                            stage = clean_line.split("stage ", 1)[1].split(" ", 1)[0]
                        record = dict(fields)
                        record["_log_node"] = node_id
                        record["stage"] = stage
                        record["step"] = parse_int(fields.get("step"), -1)
                        memory_records.append(record)
                    if "qwen3-engram-context:" not in clean_line:
                        continue
                    fields = parse_pairs(clean_line)
                    step = parse_int(fields.get("step"), context_step)
                    record = {
                        "_log_node": node_id,
                        "step": step,
                        "mode": fields.get("mode", ""),
                        "output_checksum": fields.get("output_checksum", "0x0"),
                        "gate_checksum": fields.get("gate_checksum", "0x0"),
                        "index_checksum": fields.get("index_checksum", "0x0"),
                    }
                    for key in (
                        "table_rows",
                        "output_l1_milli",
                        "latency_ms",
                        "row_prefetch_hits",
                        "row_prefetch_requests",
                        "row_prefetch_hit_rate_milli",
                        "table_bytes_moved",
                        "gate_weight_bytes_moved",
                        "indices_bytes_moved",
                        "hidden_input_bytes",
                        "hidden_output_bytes",
                        "hidden_injection_overhead_bytes",
                    ):
                        record[key] = parse_int(fields.get(key), 0)
                    engram_context_records.append(record)
                    context_step += 1

    return (
        tokens,
        timings,
        handoff_timings,
        idle_timings,
        engram_timings,
        engram_context_records,
        memory_records,
        device_records,
        boundary_observations,
        worker_events,
        barriers,
        pool_usage,
        passes,
        missing_logs,
        latest_status,
    )


def node_round_ms(record, barriers):
    barrier = barriers.get((record["_log_node"], record["step"]))
    if barrier and barrier["total_with_barrier_ms"] > 0:
        return barrier["total_with_barrier_ms"]
    return record["total_ms"]


def emit_summary(run_dir, expected_steps, node_ids, output):
    (
        tokens,
        timings,
        handoff_timings,
        idle_timings,
        engram_timings,
        engram_context_records,
        memory_records,
        device_records,
        boundary_observations,
        worker_events,
        barriers,
        pool_usage,
        passes,
        missing_logs,
        _latest_status,
    ) = parse_run_logs(
        run_dir, expected_steps, node_ids
    )
    passed_nodes = sum(1 for count in passes.values() if count >= expected_steps)
    paper_engram_context_records = [
        record for record in engram_context_records if "paper" in record.get("mode", "")
    ]
    fused_simt_context_records = [
        record
        for record in engram_context_records
        if record.get("mode", "").startswith("fused-simt")
    ]
    fused_simt_vendor_context_records = [
        record
        for record in fused_simt_context_records
        if record.get("mode", "").startswith("fused-simt-vendor")
    ]

    output.append(f"summary: run_dir={run_dir}")
    output.append(
        "summary: "
        f"decode_steps_expected={expected_steps} "
        f"decode_steps_observed={len(tokens)} "
        f"worker_timing_records={len(timings)} "
        f"passed_nodes={passed_nodes}/{len(node_ids)} "
        f"handoff_timing_records={len(handoff_timings)} "
        f"idle_timing_records={len(idle_timings)} "
        f"engram_timing_records={len(engram_timings)} "
        f"engram_context_records={len(engram_context_records)} "
        f"paper_engram_context_records={len(paper_engram_context_records)} "
        f"fused_simt_context_records={len(fused_simt_context_records)} "
        f"fused_simt_vendor_context_records={len(fused_simt_vendor_context_records)}"
    )
    if missing_logs:
        output.append(f"summary: missing_guest_logs={quote_text(missing_logs)}")

    emit_token_summary(tokens, expected_steps, output)
    emit_timing_summary(timings, idle_timings, barriers, expected_steps, node_ids, output)
    emit_handoff_timing_summary(handoff_timings, idle_timings, expected_steps, node_ids, output)
    emit_engram_timing_summary(engram_timings, expected_steps, node_ids, output)
    emit_engram_context_summary(engram_context_records, expected_steps, output)
    emit_paper_engram_context_summary(paper_engram_context_records, expected_steps, output)
    emit_fused_simt_context_summary(fused_simt_context_records, expected_steps, output)
    emit_fused_simt_vendor_context_summary(
        fused_simt_vendor_context_records, expected_steps, output
    )
    emit_memory_service_summary(memory_records, worker_events, expected_steps, output)
    emit_w5_device_summary(device_records, output)
    emit_worker_shortpath_summary(memory_records, worker_events, expected_steps, node_ids, output)
    emit_boundary_observation_summary(
        boundary_observations,
        expected_steps,
        output,
        derive_run_id_from_run_dir(run_dir),
    )
    emit_pool_usage_summary(pool_usage, expected_steps, node_ids, output, timings, idle_timings)


def derive_run_id_from_run_dir(run_dir):
    name = os.path.basename(os.path.normpath(run_dir))
    if name.endswith("_headless8"):
        return name[: -len("_headless8")]
    return name


def emit_progress(run_dir, expected_steps, elapsed_s, node_ids, output):
    (
        tokens,
        _timings,
        _handoff_timings,
        _idle_timings,
        _engram_timings,
        _engram_context_records,
        _memory_records,
        _device_records,
        _boundary_observations,
        _worker_events,
        _barriers,
        _pool_usage,
        passes,
        missing_logs,
        latest_status,
    ) = parse_run_logs(
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


def emit_timing_summary(timings, idle_timings, barriers, expected_steps, node_ids, output):
    if not timings and not idle_timings:
        output.append("timing: unavailable reason=no_qwen3_worker_timing_records")
        return

    timings_by_step = collections.defaultdict(list)
    timings_by_node = collections.defaultdict(list)
    for record in timings:
        timings_by_step[record["step"]].append(record)
        timings_by_node[record["_log_node"]].append(record)

    idle_by_step = collections.defaultdict(list)
    idle_by_node = collections.defaultdict(list)
    for record in idle_timings:
        idle_by_step[record["step"]].append(record)
        idle_by_node[record["_log_node"]].append(record)

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

    for step in sorted(idle_by_step):
        records = idle_by_step[step]
        max_idle = max(records, key=lambda item: item["input_wait_ms"])
        terminal_observed = sum(1 for record in records if record["terminal_observed"] != 0)
        output.append(
            "timing_idle_step: "
            f"step={step} "
            f"idle_nodes={len(records)}/{len(node_ids)} "
            f"terminal_observed={terminal_observed}/{len(records)} "
            f"max_terminal_wait_ms={max_idle['input_wait_ms']} "
            f"critical_node={max_idle['_log_node']} "
            "status=no_work_item"
        )

    for node_id in node_ids:
        records = sorted(timings_by_node.get(node_id, []), key=lambda item: item["step"])
        if not records:
            idle_records = sorted(idle_by_node.get(node_id, []), key=lambda item: item["step"])
            if idle_records:
                output.append(
                    "timing_node: "
                    f"node={node_id} "
                    f"steps=0/{expected_steps} "
                    f"idle_steps={len(idle_records)}/{expected_steps} "
                    f"max_terminal_wait_ms={max(record['input_wait_ms'] for record in idle_records)} "
                    "status=idle_no_work_item"
                )
                continue
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

    if step_summaries:
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


def emit_handoff_timing_summary(handoff_timings, idle_timings, expected_steps, node_ids, output):
    if not handoff_timings and not idle_timings:
        output.append("handoff_timing: unavailable reason=no_qwen3_worker_handoff_timing_records")
        return

    def is_range_handoff_edge(record):
        return record["node"] > 1 and record["source"] == record["node"] - 1

    handoffs_by_step = collections.defaultdict(list)
    handoffs_by_node = collections.defaultdict(list)
    for record in handoff_timings:
        handoffs_by_step[record["step"]].append(record)
        handoffs_by_node[record["_log_node"]].append(record)

    idle_by_node = collections.defaultdict(list)
    for record in idle_timings:
        idle_by_node[record["_log_node"]].append(record)

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
        edge_records = [record for record in records if is_range_handoff_edge(record)]
        if edge_records:
            max_edge = max(edge_records, key=lambda item: item["producer_to_input_found_mono_ms"])
            min_edge = min(edge_records, key=lambda item: item["producer_to_input_found_mono_ms"])
            max_wait = max(edge_records, key=lambda item: item["input_wait_attempts"])
            total_edge_raw_ms = sum(
                record["producer_to_input_found_mono_ms"] for record in edge_records
            )
            total_edge_clamped_ms = sum(
                max(0, record["producer_to_input_found_mono_ms"])
                for record in edge_records
            )
            total_metadata_ms = sum(record["input_metadata_ms"] for record in edge_records)
            total_activate_ms = sum(record["input_activate_ms"] for record in edge_records)
            output.append(
                "edge_step: "
                f"step={step} "
                f"edges={len(edge_records)}/{max(0, len(node_ids) - 1)} "
                f"total_edge_gap_mono_ms={total_edge_clamped_ms} "
                f"total_edge_gap_mono_raw_ms={total_edge_raw_ms} "
                f"max_edge_gap_mono_ms={max_edge['producer_to_input_found_mono_ms']} "
                f"max_edge={max_edge['source']}->{max_edge['node']} "
                f"min_edge_gap_mono_ms={min_edge['producer_to_input_found_mono_ms']} "
                f"min_edge={min_edge['source']}->{min_edge['node']} "
                f"metadata_ms={total_metadata_ms} "
                f"activate_ms={total_activate_ms} "
                f"max_wait_attempts={max_wait['input_wait_attempts']} "
                f"max_wait_edge={max_wait['source']}->{max_wait['node']}"
            )

    edge_records = [record for record in handoff_timings if is_range_handoff_edge(record)]
    gsva_records = [
        record
        for record in handoff_timings
        if record.get("kv_backend") == "gsva"
        or record.get("gsva_lookup_ms", 0) > 0
        or record.get("gsva_map_read_ms", 0) > 0
    ]
    if handoff_timings:
        max_handoff_record = max(
            handoff_timings,
            key=lambda item: item["input_found_to_handoff_ms"],
        )
        max_kv_record = max(
            handoff_timings, key=lambda item: item["kv_resolve_ms"] + item["kv_load_ms"]
        )
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
    if gsva_records:
        max_lookup_record = max(gsva_records, key=lambda item: item["gsva_lookup_ms"])
        max_map_read_record = max(gsva_records, key=lambda item: item["gsva_map_read_ms"])
        output.append(
            "gsva_timing: "
            f"records={len(gsva_records)} "
            f"lookup_ms={sum(record['gsva_lookup_ms'] for record in gsva_records)} "
            f"map_read_ms={sum(record['gsva_map_read_ms'] for record in gsva_records)} "
            f"avoided_compute_ms={sum(record['prefix_cache_avoided_compute_ms'] for record in gsva_records)} "
            f"max_lookup_step={max_lookup_record['step']} "
            f"max_lookup_node={max_lookup_record['_log_node']} "
            f"max_lookup_ms={max_lookup_record['gsva_lookup_ms']} "
            f"max_map_read_step={max_map_read_record['step']} "
            f"max_map_read_node={max_map_read_record['_log_node']} "
            f"max_map_read_ms={max_map_read_record['gsva_map_read_ms']}"
        )
    if edge_records:
        max_edge_record = max(edge_records, key=lambda item: item["producer_to_input_found_mono_ms"])
        max_wait_record = max(edge_records, key=lambda item: item["input_wait_attempts"])
        output.append(
            "edge_bottleneck: "
            f"max_edge_step={max_edge_record['step']} "
            f"edge={max_edge_record['source']}->{max_edge_record['node']} "
            f"node={max_edge_record['_log_node']} "
            f"producer_to_input_found_mono_ms={max_edge_record['producer_to_input_found_mono_ms']} "
            f"producer_to_input_found_supernode_ms={max_edge_record['producer_to_input_found_supernode_ms']} "
            f"input_wait_attempts={max_edge_record['input_wait_attempts']}"
        )
        output.append(
            "edge_bottleneck: "
            f"max_wait_step={max_wait_record['step']} "
            f"edge={max_wait_record['source']}->{max_wait_record['node']} "
            f"node={max_wait_record['_log_node']} "
            f"input_wait_attempts={max_wait_record['input_wait_attempts']} "
            f"producer_to_input_found_mono_ms={max_wait_record['producer_to_input_found_mono_ms']} "
            f"input_metadata_ms={max_wait_record['input_metadata_ms']} "
            f"input_activate_ms={max_wait_record['input_activate_ms']}"
        )

    for node_id in node_ids:
        records = sorted(handoffs_by_node.get(node_id, []), key=lambda item: item["step"])
        if not records:
            idle_records = sorted(idle_by_node.get(node_id, []), key=lambda item: item["step"])
            if idle_records:
                output.append(
                    "handoff_node: "
                    f"node={node_id} "
                    f"steps=0/{expected_steps} "
                    f"idle_steps={len(idle_records)}/{expected_steps} "
                    f"max_terminal_wait_ms={max(record['input_wait_ms'] for record in idle_records)} "
                    "status=idle_no_work_item"
                )
                continue
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


def emit_engram_timing_summary(engram_timings, expected_steps, node_ids, output):
    if not engram_timings:
        output.append("engram_timing: unavailable reason=no_qwen3_engram_timing_records")
        return

    timing_keys = (
        "candidate_publish_ms",
        "candidate_wait_ms",
        "policy_select_ms",
        "decision_publish_ms",
        "selected_wait_ms",
        "selected_writeback_ms",
        "history_state_wait_ms",
        "qwen3_range_publish_ms",
        "qwen3_range_input_wait_ms",
    )
    engram_keys = (
        "candidate_publish_ms",
        "candidate_wait_ms",
        "policy_select_ms",
        "decision_publish_ms",
        "selected_wait_ms",
        "selected_writeback_ms",
        "history_state_wait_ms",
    )
    transport_keys = (
        "candidate_publish_ms",
        "candidate_wait_ms",
        "decision_publish_ms",
        "selected_wait_ms",
        "selected_writeback_ms",
        "history_state_wait_ms",
    )

    timings_by_step = collections.defaultdict(list)
    timings_by_node = collections.defaultdict(list)
    for record in engram_timings:
        timings_by_step[record["step"]].append(record)
        timings_by_node[record["_log_node"]].append(record)

    for step in sorted(timings_by_step):
        records = timings_by_step[step]
        idle_records = [record for record in records if record.get("work_item") == "none"]
        totals = {key: sum(record[key] for record in records) for key in timing_keys}
        max_range_input = max(record["qwen3_range_input_wait_ms"] for record in records)
        max_range_publish = max(record["qwen3_range_publish_ms"] for record in records)
        engram_total = sum(totals[key] for key in engram_keys)
        transport_total = sum(totals[key] for key in transport_keys)
        policy_total = totals["policy_select_ms"]
        range_pipeline_total = max_range_input + max_range_publish
        categories = {
            "cpu_policy": policy_total,
            "object_transport": transport_total,
            "range_pipeline": range_pipeline_total,
        }
        bottleneck_name, bottleneck_ms = max(categories.items(), key=lambda item: item[1])
        output.append(
            "engram_timing_step: "
            f"step={step} "
            f"nodes={len(records)}/{len(node_ids)} "
            f"candidate_publish_ms={totals['candidate_publish_ms']} "
            f"candidate_wait_ms={totals['candidate_wait_ms']} "
            f"policy_select_ms={totals['policy_select_ms']} "
            f"decision_publish_ms={totals['decision_publish_ms']} "
            f"selected_wait_ms={totals['selected_wait_ms']} "
            f"selected_writeback_ms={totals['selected_writeback_ms']} "
            f"history_state_wait_ms={totals['history_state_wait_ms']} "
            f"engram_total_ms={engram_total} "
            f"max_qwen3_range_publish_ms={max_range_publish} "
            f"max_qwen3_range_input_wait_ms={max_range_input} "
            f"bottleneck={bottleneck_name} "
            f"bottleneck_ms={bottleneck_ms} "
            f"idle_nodes={len(idle_records)}"
        )

    global_totals = {key: sum(record[key] for record in engram_timings) for key in timing_keys}
    max_policy = max(engram_timings, key=lambda item: item["policy_select_ms"])
    max_transport = max(
        engram_timings,
        key=lambda item: sum(item[key] for key in transport_keys),
    )
    max_range = max(
        engram_timings,
        key=lambda item: item["qwen3_range_input_wait_ms"] + item["qwen3_range_publish_ms"],
    )
    policy_total = global_totals["policy_select_ms"]
    transport_total = sum(global_totals[key] for key in transport_keys)
    range_pipeline_total = sum(
        max(record["qwen3_range_input_wait_ms"] + record["qwen3_range_publish_ms"]
            for record in records)
        for records in timings_by_step.values()
    )
    categories = {
        "cpu_policy": policy_total,
        "object_transport": transport_total,
        "range_pipeline": range_pipeline_total,
    }
    bottleneck_name, bottleneck_ms = max(categories.items(), key=lambda item: item[1])
    output.append(
        "engram_bottleneck: "
        f"dominant={bottleneck_name} "
        f"dominant_ms={bottleneck_ms} "
        f"cpu_policy_ms={policy_total} "
        f"object_transport_ms={transport_total} "
        f"range_pipeline_ms={range_pipeline_total}"
    )
    output.append(
        "engram_bottleneck: "
        f"max_policy_step={max_policy['step']} "
        f"node={max_policy['_log_node']} "
        f"policy_select_ms={max_policy['policy_select_ms']}"
    )
    output.append(
        "engram_bottleneck: "
        f"max_transport_step={max_transport['step']} "
        f"node={max_transport['_log_node']} "
        f"object_transport_ms={sum(max_transport[key] for key in transport_keys)}"
    )
    output.append(
        "engram_bottleneck: "
        f"max_range_step={max_range['step']} "
        f"node={max_range['_log_node']} "
        f"qwen3_range_input_wait_ms={max_range['qwen3_range_input_wait_ms']} "
        f"qwen3_range_publish_ms={max_range['qwen3_range_publish_ms']}"
    )

    for node_id in node_ids:
        records = sorted(timings_by_node.get(node_id, []), key=lambda item: item["step"])
        if not records:
            output.append(f"engram_timing_node: node={node_id} steps=0/{expected_steps} status=missing")
            continue
        idle_steps = sum(1 for record in records if record.get("work_item") == "none")
        output.append(
            "engram_timing_node: "
            f"node={node_id} "
            f"steps={len(records)}/{expected_steps} "
            f"candidate_publish_ms={sum(record['candidate_publish_ms'] for record in records)} "
            f"candidate_wait_ms={sum(record['candidate_wait_ms'] for record in records)} "
            f"policy_select_ms={sum(record['policy_select_ms'] for record in records)} "
            f"decision_publish_ms={sum(record['decision_publish_ms'] for record in records)} "
            f"selected_wait_ms={sum(record['selected_wait_ms'] for record in records)} "
            f"selected_writeback_ms={sum(record['selected_writeback_ms'] for record in records)} "
            f"history_state_wait_ms={sum(record['history_state_wait_ms'] for record in records)} "
            f"max_qwen3_range_input_wait_ms={max(record['qwen3_range_input_wait_ms'] for record in records)} "
            f"idle_steps={idle_steps}"
        )


def emit_engram_context_summary(engram_context_records, expected_steps, output):
    emit_context_record_summary(
        engram_context_records,
        expected_steps,
        output,
        "engram_context",
    )


def emit_paper_engram_context_summary(paper_engram_context_records, expected_steps, output):
    emit_context_record_summary(
        paper_engram_context_records,
        expected_steps,
        output,
        "paper_engram_context",
    )


def emit_fused_simt_context_summary(fused_simt_context_records, expected_steps, output):
    emit_context_record_summary(
        fused_simt_context_records,
        expected_steps,
        output,
        "fused_simt_context",
    )


def emit_fused_simt_vendor_context_summary(
    fused_simt_vendor_context_records, expected_steps, output
):
    emit_context_record_summary(
        fused_simt_vendor_context_records,
        expected_steps,
        output,
        "fused_simt_vendor_context",
    )


def emit_context_record_summary(context_records, expected_steps, output, prefix):
    if not context_records:
        return

    records = sorted(
        context_records,
        key=lambda item: (item["step"], item["_log_node"]),
    )
    modes = sorted({record["mode"] for record in records if record["mode"]})
    observed_steps = sorted({record["step"] for record in records})
    total_latency_ms = sum(record["latency_ms"] for record in records)
    max_latency = max(records, key=lambda item: item["latency_ms"])
    row_prefetch_hits = sum(record["row_prefetch_hits"] for record in records)
    row_prefetch_requests = sum(record["row_prefetch_requests"] for record in records)
    row_prefetch_hit_rate_milli = (
        row_prefetch_hits * 1000 // row_prefetch_requests
        if row_prefetch_requests
        else 0
    )
    table_bytes_moved = sum(record["table_bytes_moved"] for record in records)
    gate_weight_bytes_moved = sum(record["gate_weight_bytes_moved"] for record in records)
    indices_bytes_moved = sum(record["indices_bytes_moved"] for record in records)
    hidden_input_bytes = sum(record["hidden_input_bytes"] for record in records)
    hidden_output_bytes = sum(record["hidden_output_bytes"] for record in records)
    hidden_injection_overhead_bytes = sum(
        record["hidden_injection_overhead_bytes"] for record in records
    )
    checksum_xor = 0
    for record in records:
        checksum_xor ^= parse_int(record["output_checksum"], 0)

    output.append(
        f"{prefix}_summary: "
        f"records={len(records)} "
        f"steps={len(observed_steps)}/{expected_steps} "
        f"modes={','.join(modes)} "
        f"max_latency_ms={max_latency['latency_ms']} "
        f"max_latency_step={max_latency['step']} "
        f"max_latency_node={max_latency['_log_node']} "
        f"total_latency_ms={total_latency_ms} "
        f"output_checksum_xor=0x{checksum_xor:016x} "
        f"row_prefetch_hits={row_prefetch_hits} "
        f"row_prefetch_requests={row_prefetch_requests} "
        f"row_prefetch_hit_rate_milli={row_prefetch_hit_rate_milli} "
        f"table_bytes_moved={table_bytes_moved} "
        f"gate_weight_bytes_moved={gate_weight_bytes_moved} "
        f"indices_bytes_moved={indices_bytes_moved} "
        f"hidden_input_bytes={hidden_input_bytes} "
        f"hidden_output_bytes={hidden_output_bytes} "
        f"hidden_injection_overhead_bytes={hidden_injection_overhead_bytes}"
    )
    for record in records:
        output.append(
            f"{prefix}_step: "
            f"step={record['step']} "
            f"node={record['_log_node']} "
            f"mode={record['mode']} "
            f"table_rows={record['table_rows']} "
            f"output_checksum={record['output_checksum']} "
            f"gate_checksum={record['gate_checksum']} "
            f"index_checksum={record['index_checksum']} "
            f"output_l1_milli={record['output_l1_milli']} "
            f"latency_ms={record['latency_ms']} "
            f"row_prefetch_hits={record['row_prefetch_hits']} "
            f"row_prefetch_requests={record['row_prefetch_requests']} "
            f"row_prefetch_hit_rate_milli={record['row_prefetch_hit_rate_milli']} "
            f"table_bytes_moved={record['table_bytes_moved']} "
            f"gate_weight_bytes_moved={record['gate_weight_bytes_moved']} "
            f"indices_bytes_moved={record['indices_bytes_moved']} "
            f"hidden_input_bytes={record['hidden_input_bytes']} "
            f"hidden_output_bytes={record['hidden_output_bytes']} "
            f"hidden_injection_overhead_bytes={record['hidden_injection_overhead_bytes']}"
        )


def emit_memory_service_summary(memory_records, worker_events, expected_steps, output):
    if not memory_records:
        return

    stages = collections.Counter(record["stage"] for record in memory_records)
    observed_steps = sorted(
        {record["step"] for record in memory_records if record["step"] >= 0}
    )
    boundary_records = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_boundary_decision"
    ]
    lookup_hits = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_memory_service_boundary_lookup_response"
        and record.get("action") == "jump-to-terminal"
        and record.get("status") == "hit"
    ]
    prefix_cache_kv_hits = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_prefix_cache_kv_loaded"
    ]
    gsva_kv_reads = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_gsva_kv_loaded"
    ]
    gsva_kv_writebacks = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_gsva_kv_writeback"
    ]
    prefix_cache_gsva_rejections = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_prefix_cache_gsva_rejected"
    ]
    prefix_cache_suffix_replays = [
        record
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_prefix_cache_suffix_replay_token"
    ]
    prefix_cache_recompute_range_forwards = (
        worker_events["range_forwards"] if prefix_cache_gsva_rejections else 0
    )
    prefix_cache_reject_policy = (
        "cache_reject_then_recompute" if prefix_cache_gsva_rejections else "none"
    )
    prefix_cache_reject_then_recompute = (
        1
        if prefix_cache_gsva_rejections
        and not gsva_kv_reads
        and prefix_cache_recompute_range_forwards > 0
        else 0
    )
    output.append(
        "memory_service_summary: "
        "service=lingqu_memory_service "
        f"records={len(memory_records)} "
        f"steps={len(observed_steps)}/{expected_steps} "
        f"stages={','.join(f'{stage}:{count}' for stage, count in sorted(stages.items()))} "
        f"shortpath_ids={csv_or_none(record.get('shortpath_id') for record in memory_records)} "
        f"support_ids={csv_or_none(record.get('shortpath_support_id') for record in memory_records)} "
        f"actions={csv_or_none(record.get('shortpath_action') for record in memory_records)} "
        f"artifact_kinds={csv_or_none(record.get('shortpath_artifact_kind') for record in memory_records)} "
        f"prefetch_ids={csv_or_none(record.get('prefetch_id') for record in memory_records)} "
        f"prefix_cache_ids={csv_or_none(record.get('prefix_cache_id') for record in memory_records)} "
        f"prefix_cache_actions={csv_or_none(record.get('prefix_cache_action') for record in memory_records)} "
        f"prefix_cache_kv_hits={len(prefix_cache_kv_hits)} "
        f"prefix_cache_kv_nodes={csv_or_none_ordered(record.get('node') for record in prefix_cache_kv_hits)} "
        f"prefix_cache_gsva_rejections={len(prefix_cache_gsva_rejections)} "
        f"prefix_cache_gsva_rejection_reasons={csv_or_none(record.get('reason') for record in prefix_cache_gsva_rejections)} "
        f"gsva_kv_refs={len(gsva_kv_reads) + len(gsva_kv_writebacks)} "
        f"gsva_reads={len(gsva_kv_reads)} "
        f"gsva_writebacks={len(gsva_kv_writebacks)} "
        f"gsva_kv_nodes={csv_or_none_ordered(record.get('node') for record in gsva_kv_reads + gsva_kv_writebacks)} "
        f"lookup_hits={len(lookup_hits)} "
        f"hit_registry_indexes={csv_or_none_ordered(record.get('registry_index') for record in lookup_hits)} "
        f"hit_registry_steps={csv_or_none_ordered(lookup_hit_registry_step(record) for record in lookup_hits)} "
        f"hit_positions={csv_or_none_ordered(record.get('position') for record in lookup_hits)} "
        f"prefix_cache_reject_policy={prefix_cache_reject_policy} "
        f"prefix_cache_recompute_range_forwards={prefix_cache_recompute_range_forwards} "
        f"prefix_cache_reject_then_recompute={prefix_cache_reject_then_recompute} "
        f"prefix_cache_matched_tokens={csv_or_none(record.get('prefix_cache_matched_tokens') for record in memory_records)} "
        f"prefix_cache_suffix_replay_tokens={len(prefix_cache_suffix_replays)} "
        f"prefix_cache_suffix_replay_steps={csv_or_none_ordered(record.get('step') for record in prefix_cache_suffix_replays)}"
    )

    if boundary_records:
        by_step = collections.defaultdict(list)
        for record in boundary_records:
            by_step[record["step"]].append(record)
        hits_by_step = collections.defaultdict(list)
        for record in lookup_hits:
            hits_by_step[record["step"]].append(record)
        for step in sorted(by_step):
            records = by_step[step]
            step_hits = hits_by_step.get(step, [])
            output.append(
                "memory_service_step: "
                f"step={step} "
                f"boundary_records={len(records)} "
                f"nodes={csv_or_none(record.get('local') for record in records)} "
                f"shortpath_ids={csv_or_none(record.get('shortpath_id') for record in records)} "
                f"support_ids={csv_or_none(record.get('shortpath_support_id') for record in records)} "
                f"actions={csv_or_none(record.get('shortpath_action') for record in records)} "
                f"prefetch_ids={csv_or_none(record.get('prefetch_id') for record in records)} "
                f"prefix_cache_ids={csv_or_none(record.get('prefix_cache_id') for record in records)} "
                f"prefix_cache_actions={csv_or_none(record.get('prefix_cache_action') for record in records)} "
                f"lookup_hits={len(step_hits)} "
                f"hit_registry_indexes={csv_or_none_ordered(record.get('registry_index') for record in step_hits)} "
                f"hit_registry_steps={csv_or_none_ordered(lookup_hit_registry_step(record) for record in step_hits)} "
                f"hit_positions={csv_or_none_ordered(record.get('position') for record in step_hits)} "
                f"prefix_cache_matched_tokens={csv_or_none(record.get('prefix_cache_matched_tokens') for record in records)}"
            )


def emit_w5_device_summary(device_records, output):
    if not device_records:
        return

    tensor_consumers = [
        record
        for record in device_records
        if record["stage"] == "qwen3_w5_device_gsva_tensor_consumer"
    ]
    rejected = [
        record
        for record in device_records
        if record["stage"] == "qwen3_w5_device_gsva_tensor_rejected"
    ]
    checksum_matches = [
        record
        for record in tensor_consumers
        if parse_int(record.get("cpu_checksum")) != 0
        and parse_int(record.get("cpu_checksum")) == parse_int(record.get("device_checksum"))
    ]
    shape_verified = [
        record
        for record in tensor_consumers
        if parse_int(record.get("output_shape")) > 0
        and parse_int(record.get("output_bytes")) > 0
    ]
    if not tensor_consumers:
        status = "no_consumer"
    elif len(checksum_matches) == len(tensor_consumers) and len(shape_verified) == len(
        tensor_consumers
    ):
        status = "ok"
    else:
        status = "mismatch"

    output.append(
        "w5_device_summary: "
        f"records={len(device_records)} "
        f"tensor_consumers={len(tensor_consumers)} "
        f"devices={csv_or_none_ordered(record.get('device') for record in device_records)} "
        f"backends={csv_or_none_ordered(record.get('backend') for record in device_records)} "
        f"ops={csv_or_none_ordered(record.get('op') for record in tensor_consumers)} "
        f"nodes={csv_or_none_ordered(record.get('node') for record in device_records)} "
        f"output_shapes={csv_or_none_ordered(record.get('output_shape') for record in tensor_consumers)} "
        f"checksum_matches={len(checksum_matches)} "
        f"shape_verified={len(shape_verified)} "
        f"rejections={len(rejected)} "
        f"rejection_guards={csv_or_none_ordered(record.get('guard') for record in rejected)} "
        f"rejection_reasons={csv_or_none_ordered(record.get('reason') for record in rejected)} "
        f"status={status}"
    )


def emit_worker_shortpath_summary(memory_records, worker_events, expected_steps, node_ids, output):
    if not memory_records:
        return

    boundary_hits = sum(
        1
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_terminal_logits_loaded"
    )
    terminal_selects = sum(
        1
        for record in memory_records
        if record["stage"] == "qwen3_w5_memory_terminal_logits_selected"
    )
    if boundary_hits == 0 and terminal_selects == 0:
        return

    full_pipeline_range_forwards = expected_steps * len(node_ids)
    full_pipeline_runtime_inputs = max(0, full_pipeline_range_forwards - 1)
    full_pipeline_runtime_outputs = full_pipeline_range_forwards
    output.append(
        "guest_worker_shortpath_summary: "
        "action=jump-to-terminal "
        f"boundary_hits={boundary_hits} "
        f"terminal_selects={terminal_selects} "
        f"expected_hits={expected_steps} "
        f"actual_range_forwards={worker_events['range_forwards']} "
        f"actual_runtime_inputs={worker_events['runtime_inputs']} "
        f"actual_runtime_outputs={worker_events['runtime_outputs']} "
        f"shortpath_no_dispatch={worker_events['shortpath_no_dispatches']} "
        f"shortpath_terminal_commits={worker_events['shortpath_terminal_commits']} "
        f"shortpath_publish_hidden_zero={worker_events['shortpath_publish_hidden_zero']} "
        f"full_pipeline_range_forwards={full_pipeline_range_forwards} "
        f"full_pipeline_runtime_inputs={full_pipeline_runtime_inputs} "
        f"full_pipeline_runtime_outputs={full_pipeline_runtime_outputs}"
    )


def emit_boundary_observation_summary(
    boundary_observations, expected_steps, output, run_id
):
    if not boundary_observations:
        return

    observed_steps = sorted({record["step"] for record in boundary_observations})
    output.append(
        "memory_boundary_observation_summary: "
        f"records={len(boundary_observations)} "
        f"steps={len(observed_steps)}/{expected_steps} "
        f"nodes={csv_or_none(record.get('node') for record in boundary_observations)} "
        f"targets={csv_or_none(record.get('target') for record in boundary_observations)} "
        "source=w5_guest_range_exit "
        "hidden_backend=obmm_shmem"
    )
    for record in sorted(
        boundary_observations, key=lambda item: (item["step"], item["node"])
    ):
        observation_id = record.get("observation_id") or (
            f"boundary-observation/{run_id}/step{record['step']}/{record['node']}"
        )
        output.append(
            "memory_boundary_observation: "
            "phase=range_exit "
            f"observation_id={observation_id} "
            f"step={record['step']} "
            f"node={record['node']} "
            f"target={record['target']} "
            f"layers={record['layers']} "
            f"layer_start={record['layer_start']} "
            f"layer_end={record['layer_end']} "
            f"layer_count={record['layer_count']} "
            f"hidden_key={record['hidden_key']} "
            f"hidden_key_hash={record['hidden_key_hash']} "
            f"hidden_version={record['hidden_version']} "
            f"hidden_bytes={record['hidden_bytes']} "
            f"hidden_checksum={record['hidden_checksum']} "
            f"hidden_dtype={record['hidden_dtype']} "
            f"hidden_shape={record['hidden_shape']} "
            f"producer_publish_ms={record['producer_publish_ms']} "
            f"producer_publish_mono_ms={record['producer_publish_mono_ms']} "
            f"backing={record['backing']} "
            f"metadata={record['metadata']} "
            f"queue={record['queue']} "
            f"status={record['status']}"
        )


def emit_pool_usage_summary(
    pool_usage,
    expected_steps,
    node_ids,
    output,
    timings=None,
    idle_timings=None,
):
    if not pool_usage:
        active_records = len(timings or [])
        idle_records = len(idle_timings or [])
        output.append(
            "obmm_pool: not_observed reason=no_qwen3_obmm_pool_usage_records "
            f"active_worker_records={active_records} "
            f"idle_worker_records={idle_records}"
        )
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
