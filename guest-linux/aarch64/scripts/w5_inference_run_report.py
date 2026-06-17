#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from pathlib import Path


PAIR_RE = re.compile(r"([A-Za-z0-9_]+)=([^ \r\n]+)")
RUN_SUMMARY_RE = re.compile(r"^eight_node_w5_inference_cluster_summary\.(.+)\.txt$")
TOKEN_IDS_RE = re.compile(r"^decode_output: token_ids=(\[.*\])$")

BAD_MARKERS = (
    "status=missing",
    "qwen3_range_kv_state_lazy_fallback",
    "fallback=runtime_forward_metadata",
    "obmm_pool: unavailable",
)

ARTIFACT_LIMITS = {
    "memory_store_json": 16 * 1024 * 1024,
    "memory_store_bin": 256 * 1024 * 1024,
    "object_store_json": 8 * 1024 * 1024,
    "object_store_bin": 256 * 1024 * 1024,
    "shortpath_stream": 1024 * 1024,
    "shortpath_kv_stream": 1024 * 1024,
    "prefix_cache_kv_stream": 1024 * 1024,
}

OPTIONAL_ARTIFACTS = {
    "memory_store_bin",
}

CONTEXT_SUMMARY_PREFIXES = (
    "engram_context",
    "paper_engram_context",
    "fused_simt_context",
    "fused_simt_vendor_context",
)


def byte_level_decoder():
    visible = (
        list(range(ord("!"), ord("~") + 1))
        + list(range(0xA1, 0xAC + 1))
        + list(range(0xAE, 0xFF + 1))
    )
    byte_values = visible[:]
    codepoints = visible[:]
    next_codepoint = 0
    for value in range(256):
        if value in visible:
            continue
        byte_values.append(value)
        codepoints.append(256 + next_codepoint)
        next_codepoint += 1
    return {chr(codepoint): value for value, codepoint in zip(byte_values, codepoints)}


BYTE_LEVEL_DECODER = byte_level_decoder()


def split_env_regexes(value):
    if not value:
        return []
    stripped = value.strip()
    if not stripped:
        return []
    if stripped.startswith("["):
        try:
            parsed = json.loads(stripped)
        except json.JSONDecodeError:
            return [stripped]
        return [str(item) for item in parsed if str(item)]
    return [line.strip() for line in stripped.splitlines() if line.strip()]


def split_env_values(value):
    if not value:
        return []
    stripped = value.strip()
    if not stripped:
        return []
    if stripped.startswith("["):
        try:
            parsed = json.loads(stripped)
        except json.JSONDecodeError:
            return [stripped]
        return [str(item) for item in parsed if str(item)]
    normalized = stripped.replace(",", "\n")
    return [line.strip() for line in normalized.splitlines() if line.strip()]


def output_guard_from_env():
    return {
        "tokenizer_dir": os.environ.get("SIM_W5_OUTPUT_TOKENIZER_DIR")
        or os.environ.get("SIM_QWEN3_DENSE_WEIGHTS_PATH")
        or "",
        "expect_regexes": split_env_regexes(os.environ.get("SIM_W5_EXPECT_OUTPUT_REGEX")),
        "reject_regexes": split_env_regexes(os.environ.get("SIM_W5_REJECT_OUTPUT_REGEX")),
    }


def output_guard_from_args(args):
    guard = output_guard_from_env()
    if args.tokenizer_dir is not None:
        guard["tokenizer_dir"] = str(args.tokenizer_dir)
    guard["expect_regexes"].extend(args.expect_output_regex or [])
    guard["reject_regexes"].extend(args.reject_output_regex or [])
    return guard


def context_guard_from_env():
    return {
        "require_contexts": split_env_values(os.environ.get("SIM_W5_REQUIRE_CONTEXT")),
    }


def context_guard_from_args(args):
    guard = context_guard_from_env()
    guard["require_contexts"].extend(args.require_context or [])
    return guard


def prefix_cache_guard_from_args(args):
    return bool(
        os.environ.get("SIM_W5_REQUIRE_PREFIX_CACHE") in ("1", "true", "TRUE", "yes", "YES")
        or args.require_prefix_cache
    )


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


def parse_decode_token_ids(parsed):
    for line in parsed["decode_output"]:
        match = TOKEN_IDS_RE.match(line)
        if not match:
            continue
        try:
            values = json.loads(match.group(1))
        except json.JSONDecodeError as error:
            return [], f"decode token_ids are not valid JSON: {error.msg}"
        if not isinstance(values, list):
            return [], "decode token_ids is not a list"
        token_ids = []
        for value in values:
            if not isinstance(value, int):
                return [], f"decode token_ids contains non-integer value={value!r}"
            token_ids.append(value)
        return token_ids, ""
    return [], "missing decode_output token_ids"


def tokenizer_json_path(tokenizer_dir):
    path = Path(tokenizer_dir)
    if path.is_file():
        return path
    return path / "tokenizer.json"


def decode_byte_level_text(text):
    raw = bytearray()
    for char in text:
        value = BYTE_LEVEL_DECODER.get(char)
        if value is None:
            raw.extend(char.encode("utf-8"))
        else:
            raw.append(value)
    return raw.decode("utf-8", errors="replace")


def decode_tokenizer_piece_text(text, tokenizer):
    decoder = tokenizer.get("decoder", {})
    if isinstance(decoder, dict) and decoder.get("type") == "ByteLevel":
        return decode_byte_level_text(text)
    return text


def decode_output_text(token_ids, tokenizer_dir):
    if not tokenizer_dir:
        return "", "missing tokenizer dir"
    path = tokenizer_json_path(tokenizer_dir)
    try:
        tokenizer = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return "", f"tokenizer.json missing: {path}"
    except json.JSONDecodeError as error:
        return "", f"tokenizer.json is not valid JSON: {error.msg}"
    vocab = tokenizer.get("model", {}).get("vocab", {})
    if not isinstance(vocab, dict):
        return "", "tokenizer model.vocab is missing"
    by_id = {}
    for piece, token_id in vocab.items():
        if isinstance(token_id, int):
            by_id[token_id] = piece
    pieces = []
    missing = []
    for token_id in token_ids:
        piece = by_id.get(token_id)
        if piece is None:
            missing.append(token_id)
        else:
            pieces.append(piece)
    if missing:
        return "", f"tokenizer missing token ids: {missing[:8]}"
    return decode_tokenizer_piece_text("".join(pieces), tokenizer), ""


def evaluate_output_guard(parsed, output_guard):
    guard = output_guard or {}
    expect_regexes = list(guard.get("expect_regexes") or [])
    reject_regexes = list(guard.get("reject_regexes") or [])
    enabled = bool(expect_regexes or reject_regexes)
    result = {
        "enabled": enabled,
        "status": "disabled",
        "tokenizer": str(guard.get("tokenizer_dir") or ""),
        "text": "",
        "expect_regexes": expect_regexes,
        "reject_regexes": reject_regexes,
        "issues": [],
    }
    if not enabled:
        return result

    token_ids, error = parse_decode_token_ids(parsed)
    if error:
        result["issues"].append(error)
    else:
        text, error = decode_output_text(token_ids, result["tokenizer"])
        if error:
            result["issues"].append(error)
        else:
            result["text"] = text

    if result["text"]:
        for pattern in expect_regexes:
            if not re.search(pattern, result["text"]):
                result["issues"].append(f"output text does not match expected regex: {pattern}")
        for pattern in reject_regexes:
            if re.search(pattern, result["text"]):
                result["issues"].append(f"output text rejected by regex: {pattern}")

    result["status"] = "fail" if result["issues"] else "pass"
    return result


def evaluate_context_guard(parsed, context_guard):
    guard = context_guard or {}
    required_contexts = list(guard.get("require_contexts") or [])
    expected_steps = parse_int(parsed["summary"].get("decode_steps_expected"))
    result = {
        "enabled": bool(required_contexts),
        "status": "disabled",
        "required_contexts": required_contexts,
        "issues": [],
    }
    if not required_contexts:
        return result

    for label in required_contexts:
        if label not in CONTEXT_SUMMARY_PREFIXES:
            result["issues"].append(f"unsupported required context: {label}")
            continue
        fields = parsed["context_summaries"].get(label, {})
        if parse_int(fields.get("records")) <= 0:
            result["issues"].append(f"required context missing: {label}")
            continue
        observed_steps, total_steps = split_count(fields.get("steps", "0/0"))
        if expected_steps > 0:
            if observed_steps != expected_steps or total_steps != expected_steps:
                result["issues"].append(
                    "required context step coverage mismatch: "
                    f"{label} value={fields.get('steps', '')} "
                    f"expected={expected_steps}/{expected_steps}"
                )
        elif observed_steps <= 0 or observed_steps != total_steps:
            result["issues"].append(
                f"required context step coverage mismatch: {label} "
                f"value={fields.get('steps', '')}"
            )
        if label == "fused_simt_vendor_context":
            modes = [mode for mode in fields.get("modes", "").split(",") if mode]
            if not modes:
                result["issues"].append(
                    "required fused SIMT vendor context lacks mode evidence"
                )
            elif any(not mode.startswith("fused-simt-vendor") for mode in modes):
                result["issues"].append(
                    "required fused SIMT vendor context has non-vendor mode: "
                    f"{fields.get('modes', '')}"
                )
            for metric in (
                "row_prefetch_hits",
                "row_prefetch_requests",
                "table_bytes_moved",
                "hidden_injection_overhead_bytes",
            ):
                if metric not in fields:
                    result["issues"].append(
                        "required fused SIMT vendor context lacks metric: "
                        f"{metric}"
                    )

    result["status"] = "fail" if result["issues"] else "pass"
    return result


def evaluate_prefix_cache_guard(parsed, require_prefix_cache=False):
    memory = parsed["memory_service"]
    result = {
        "enabled": bool(require_prefix_cache),
        "required": bool(require_prefix_cache),
        "status": "disabled",
        "issues": [],
    }
    if not require_prefix_cache:
        return result

    if not memory:
        result["issues"].append(
            "required prefix-cache evidence missing: memory_service_summary"
        )
        result["status"] = "fail"
        return result

    prefix_cache_ids = memory.get("prefix_cache_ids", "")
    prefix_cache_actions = memory.get("prefix_cache_actions", "")
    prefix_cache_kv_hits = parse_int(memory.get("prefix_cache_kv_hits"))
    prefix_cache_gsva_rejections = parse_int(memory.get("prefix_cache_gsva_rejections"))

    if not prefix_cache_ids or prefix_cache_ids == "none":
        result["issues"].append(
            "required prefix-cache evidence missing: prefix_cache_ids=none"
        )
    if prefix_cache_actions != "reuse":
        result["issues"].append(
            f"required prefix-cache action mismatch: {prefix_cache_actions or ''}"
        )
    if prefix_cache_kv_hits <= 0:
        result["issues"].append(
            "required prefix-cache reuse has no KV hits: "
            f"prefix_cache_kv_hits={prefix_cache_kv_hits}"
        )
    if prefix_cache_gsva_rejections > 0:
        result["issues"].append(
            "required prefix-cache reuse was rejected by GSVA: "
            f"rejections={prefix_cache_gsva_rejections}"
        )

    result["status"] = "fail" if result["issues"] else "pass"
    return result


def infer_run_id(summary_path):
    match = RUN_SUMMARY_RE.match(summary_path.name)
    if not match:
        return ""
    return match.group(1)


def artifact_paths(summary_path, run_id, run_dir):
    out_dir = summary_path.parent
    memory_store = os.environ.get("SIM_W5_MEMORY_STORE")
    object_store = os.environ.get("SIM_W5_MEMORY_OBJECT_STORE")
    object_snapshot = os.environ.get("SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT")
    registry_store = os.environ.get("SIM_W5_MEMORY_REGISTRY_DIR")
    registry_dir = (
        Path(registry_store)
        if registry_store
        else out_dir / f"w5_memory_registry.{run_id}" if run_id else None
    )
    object_store_path = (
        Path(object_store)
        if object_store
        else out_dir / f"w5_object_service_store.{run_id}.json"
    )
    if object_snapshot and object_snapshot.endswith(".json") and not object_store_path.is_file():
        object_store_path = Path(object_snapshot)
        if registry_dir is not None and not registry_dir.is_dir():
            registry_dir = object_store_path.parent
    object_store_bin = (
        object_store_path.with_suffix(".bin")
        if object_store_path.suffix == ".json"
        else out_dir / f"w5_object_service_store.{run_id}.bin"
    )
    memory_store_path = (
        Path(memory_store)
        if memory_store
        else out_dir / f"w5_memory_object_store.{run_id}.json"
    )
    memory_store_bin = (
        memory_store_path.with_suffix(".bin")
        if memory_store_path.suffix == ".json"
        else out_dir / f"w5_memory_object_store.{run_id}.bin"
    )
    paths = {
        "memory_store_json": memory_store_path,
        "memory_store_bin": memory_store_bin,
        "object_store_json": object_store_path,
        "object_store_bin": object_store_bin,
    }
    if registry_dir is not None:
        paths["shortpath_stream"] = registry_dir / "w5_memory_shortpath_stream.txt"
        paths["shortpath_kv_stream"] = registry_dir / "w5_memory_shortpath_kv_stream.txt"
        paths["prefix_cache_kv_stream"] = (
            registry_dir / "w5_memory_prefix_cache_kv_stream.txt"
        )
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
        "device": {},
        "timing_steps": [],
        "timing_nodes": [],
        "gsva_timing": {},
        "engram_steps": [],
        "context_summaries": {},
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
        elif line.startswith("w5_device_summary: "):
            parsed["device"] = parse_pairs(line)
        elif line.startswith("timing_step: "):
            parsed["timing_steps"].append(parse_pairs(line))
        elif line.startswith("timing_node: "):
            parsed["timing_nodes"].append(parse_pairs(line))
        elif line.startswith("gsva_timing: "):
            parsed["gsva_timing"] = parse_pairs(line)
        elif line.startswith("engram_timing_step: "):
            parsed["engram_steps"].append(parse_pairs(line))
        else:
            for prefix in CONTEXT_SUMMARY_PREFIXES:
                marker = f"{prefix}_summary: "
                if line.startswith(marker):
                    parsed["context_summaries"][prefix] = parse_pairs(line)
                    break
        for marker in BAD_MARKERS:
            if marker in line:
                parsed["bad_markers"].append(marker)
    return parsed


def validate(
    parsed,
    paths,
    output_guard=None,
    context_guard=None,
    require_device_gsva=False,
    require_prefix_cache=False,
):
    issues = []
    summary = parsed["summary"]
    shortpath = parsed["shortpath"]
    memory = parsed["memory_service"]
    device = parsed["device"]

    expected = parse_int(summary.get("decode_steps_expected"))
    observed = parse_int(summary.get("decode_steps_observed"))
    passed_nodes = split_count(summary.get("passed_nodes", "0/0"))
    node_count = passed_nodes[1] or 8
    prefix_cache_run = bool(memory and memory.get("prefix_cache_ids") not in ("", None, "none"))
    shortpath_run = bool(
        shortpath
        or (
            memory
            and (
                memory.get("shortpath_ids") not in ("", None, "none")
                or memory.get("actions") not in ("", None, "none")
                or memory.get("artifact_kinds") not in ("", None, "none")
            )
        )
    )
    worker_expected = expected if shortpath_run else expected * node_count
    idle_expected = expected * max(node_count - 1, 0) if shortpath_run else 0

    if expected <= 0:
        issues.append("missing decode_steps_expected")
    if expected != observed:
        issues.append(f"decode steps mismatch expected={expected} observed={observed}")
    if passed_nodes[0] != passed_nodes[1] or passed_nodes[1] == 0:
        issues.append(f"passed_nodes incomplete value={summary.get('passed_nodes', '')}")
    if parse_int(summary.get("worker_timing_records")) != worker_expected:
        issues.append(
            f"worker_timing_records mismatch expected={worker_expected} "
            f"actual={summary.get('worker_timing_records', '')}"
        )
    if parse_int(summary.get("idle_timing_records")) != idle_expected:
        issues.append(
            f"idle_timing_records mismatch expected={idle_expected} "
            f"actual={summary.get('idle_timing_records', '')}"
        )

    if shortpath_run:
        if not memory:
            issues.append("missing memory_service_summary")
        else:
            memory_steps = split_count(memory.get("steps", "0/0"))
            if memory_steps != (expected, expected):
                issues.append(
                    f"memory service step coverage mismatch value={memory.get('steps', '')}"
                )
            if memory.get("actions") != "jump-to-terminal":
                issues.append(f"unexpected memory action value={memory.get('actions', '')}")
            if memory.get("artifact_kinds") != "logits":
                issues.append(
                    f"unexpected artifact_kinds value={memory.get('artifact_kinds', '')}"
                )
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

    if prefix_cache_run:
        stages = memory.get("stages", "")
        prefix_cache_actions = memory.get("prefix_cache_actions")
        prefix_cache_gsva_rejections = parse_int(
            memory.get("prefix_cache_gsva_rejections")
        )
        prefix_cache_reject_policy = memory.get("prefix_cache_reject_policy", "")
        prefix_cache_recompute_range_forwards = parse_int(
            memory.get("prefix_cache_recompute_range_forwards")
        )
        prefix_cache_reject_then_recompute = parse_int(
            memory.get("prefix_cache_reject_then_recompute")
        )
        if prefix_cache_actions not in ("reuse", "miss", "require-verify"):
            issues.append(
                f"unexpected prefix_cache_actions value={prefix_cache_actions or ''}"
            )
        if (
            prefix_cache_actions == "reuse"
            and parse_int(memory.get("prefix_cache_kv_hits")) <= 0
            and prefix_cache_gsva_rejections <= 0
        ):
            issues.append(
                f"prefix_cache_kv_hits missing value={memory.get('prefix_cache_kv_hits', '')}"
            )
        if (
            prefix_cache_actions == "reuse"
            and prefix_cache_gsva_rejections <= 0
            and "qwen3_w5_memory_prefix_cache_kv_loaded:" not in stages
        ):
            issues.append("missing prefix-cache kv loaded stage")
        if (
            prefix_cache_actions == "reuse"
            and parse_int(memory.get("gsva_kv_refs")) > 0
            and prefix_cache_gsva_rejections <= 0
            and parse_int(memory.get("gsva_reads")) <= 0
        ):
            issues.append("GSVA prefix-cache run has GSVA refs but no GSVA reads")
        if prefix_cache_gsva_rejections > 0 and memory.get(
            "prefix_cache_gsva_rejection_reasons"
        ) in ("", None, "none"):
            issues.append("GSVA prefix-cache rejection is missing reason")
        if prefix_cache_gsva_rejections > 0:
            if prefix_cache_reject_policy != "cache_reject_then_recompute":
                issues.append(
                    "GSVA prefix-cache rejection missing cache_reject_then_recompute policy"
                )
            if prefix_cache_recompute_range_forwards <= 0:
                issues.append(
                    "GSVA prefix-cache rejection missing range-forward recompute evidence"
                )
            if parse_int(memory.get("prefix_cache_kv_hits")) <= 0:
                if parse_int(memory.get("gsva_reads")) != 0:
                    issues.append(
                        "stale GSVA prefix-cache rejection still consumed GSVA reads"
                    )
                if prefix_cache_reject_then_recompute != 1:
                    issues.append(
                        "stale GSVA prefix-cache rejection missing reject-then-recompute marker"
                    )

    if require_device_gsva:
        if not device:
            issues.append("missing w5_device_summary")
        else:
            guards = {
                guard
                for guard in device.get("rejection_guards", "").split(",")
                if guard and guard != "none"
            }
            missing_guards = {"token", "epoch", "retire"} - guards
            tensor_consumers = parse_int(device.get("tensor_consumers"))
            if "npu" not in device.get("devices", "").split(","):
                issues.append(f"device summary missing npu device={device.get('devices', '')}")
            if "gsva" not in device.get("backends", "").split(","):
                issues.append(
                    f"device summary missing gsva backend={device.get('backends', '')}"
                )
            if tensor_consumers <= 0:
                issues.append("device summary has no tensor consumer")
            if parse_int(device.get("checksum_matches")) != tensor_consumers:
                issues.append(
                    "device checksum parity mismatch "
                    f"matches={device.get('checksum_matches', '')} "
                    f"consumers={device.get('tensor_consumers', '')}"
                )
            if parse_int(device.get("shape_verified")) != tensor_consumers:
                issues.append(
                    "device shape verification mismatch "
                    f"verified={device.get('shape_verified', '')} "
                    f"consumers={device.get('tensor_consumers', '')}"
                )
            if missing_guards:
                issues.append(
                    "device GSVA rejection guards missing "
                    f"value={','.join(sorted(missing_guards))}"
                )
            if device.get("status") != "ok":
                issues.append(f"device summary status={device.get('status', '')}")

    for marker in sorted(set(parsed["bad_markers"])):
        issues.append(f"bad marker present: {marker}")

    output_guard_result = evaluate_output_guard(parsed, output_guard)
    for issue in output_guard_result["issues"]:
        issues.append(f"output guard: {issue}")
    context_guard_result = evaluate_context_guard(parsed, context_guard)
    for issue in context_guard_result["issues"]:
        issues.append(f"context guard: {issue}")
    prefix_cache_guard_result = evaluate_prefix_cache_guard(
        parsed, require_prefix_cache=require_prefix_cache
    )
    for issue in prefix_cache_guard_result["issues"]:
        issues.append(f"prefix-cache guard: {issue}")

    artifact_sizes = {}
    for label, limit in ARTIFACT_LIMITS.items():
        path = paths.get(label)
        size = file_size(path) if path is not None else None
        artifact_sizes[label] = {"path": str(path) if path else "", "bytes": size, "max_bytes": limit}
        if size is None:
            required_for_shortpath = (
                shortpath_run
                and label not in OPTIONAL_ARTIFACTS
                and label != "prefix_cache_kv_stream"
            )
            required_for_prefix_cache = (
                prefix_cache_run
                and memory.get("prefix_cache_actions") == "reuse"
                and label
                in {
                    "memory_store_json",
                    "object_store_json",
                    "prefix_cache_kv_stream",
                }
            )
            if required_for_shortpath or required_for_prefix_cache:
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

    return (
        issues,
        artifact_sizes,
        output_guard_result,
        context_guard_result,
        prefix_cache_guard_result,
    )


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


def context_report(parsed):
    result = {}
    for prefix in CONTEXT_SUMMARY_PREFIXES:
        fields = parsed["context_summaries"].get(prefix, {})
        if not fields:
            continue
        result[prefix] = {
            "records": parse_int(fields.get("records")),
            "steps": fields.get("steps", ""),
            "modes": fields.get("modes", ""),
            "max_latency_ms": parse_int(fields.get("max_latency_ms")),
            "max_latency_step": parse_int(fields.get("max_latency_step"), -1),
            "max_latency_node": fields.get("max_latency_node", ""),
            "total_latency_ms": parse_int(fields.get("total_latency_ms")),
            "output_checksum_xor": fields.get("output_checksum_xor", "0x0"),
            "row_prefetch_hits": parse_int(fields.get("row_prefetch_hits")),
            "row_prefetch_requests": parse_int(fields.get("row_prefetch_requests")),
            "row_prefetch_hit_rate_milli": parse_int(
                fields.get("row_prefetch_hit_rate_milli")
            ),
            "table_bytes_moved": parse_int(fields.get("table_bytes_moved")),
            "gate_weight_bytes_moved": parse_int(fields.get("gate_weight_bytes_moved")),
            "indices_bytes_moved": parse_int(fields.get("indices_bytes_moved")),
            "hidden_input_bytes": parse_int(fields.get("hidden_input_bytes")),
            "hidden_output_bytes": parse_int(fields.get("hidden_output_bytes")),
            "hidden_injection_overhead_bytes": parse_int(
                fields.get("hidden_injection_overhead_bytes")
            ),
        }
    return result


def build_report(
    summary_path,
    output_guard=None,
    context_guard=None,
    require_device_gsva=False,
    require_prefix_cache=False,
):
    run_id = infer_run_id(summary_path)
    parsed = parse_summary(summary_path)
    paths = artifact_paths(summary_path, run_id, parsed["run_dir"])
    (
        issues,
        artifact_sizes,
        output_guard_result,
        context_guard_result,
        prefix_cache_guard_result,
    ) = validate(
        parsed,
        paths,
        output_guard,
        context_guard,
        require_device_gsva,
        require_prefix_cache,
    )
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
        "prefix_cache": {
            "ids": memory.get("prefix_cache_ids", ""),
            "actions": memory.get("prefix_cache_actions", ""),
            "kv_hits": parse_int(memory.get("prefix_cache_kv_hits")),
            "kv_nodes": memory.get("prefix_cache_kv_nodes", ""),
            "gsva_rejections": parse_int(memory.get("prefix_cache_gsva_rejections")),
            "gsva_rejection_reasons": memory.get(
                "prefix_cache_gsva_rejection_reasons", ""
            ),
            "reject_policy": memory.get("prefix_cache_reject_policy", ""),
            "recompute_range_forwards": parse_int(
                memory.get("prefix_cache_recompute_range_forwards")
            ),
            "reject_then_recompute": parse_int(
                memory.get("prefix_cache_reject_then_recompute")
            ),
        },
        "gsva": {
            "kv_refs": parse_int(memory.get("gsva_kv_refs")),
            "reads": parse_int(memory.get("gsva_reads")),
            "writebacks": parse_int(memory.get("gsva_writebacks")),
            "kv_nodes": memory.get("gsva_kv_nodes", ""),
            "timing": {
                "records": parse_int(parsed["gsva_timing"].get("records")),
                "lookup_ms": parse_int(parsed["gsva_timing"].get("lookup_ms")),
                "map_read_ms": parse_int(parsed["gsva_timing"].get("map_read_ms")),
                "avoided_compute_ms": parse_int(
                    parsed["gsva_timing"].get("avoided_compute_ms")
                ),
            },
        },
        "device": {
            "records": parse_int(parsed["device"].get("records")),
            "tensor_consumers": parse_int(parsed["device"].get("tensor_consumers")),
            "devices": parsed["device"].get("devices", ""),
            "backends": parsed["device"].get("backends", ""),
            "ops": parsed["device"].get("ops", ""),
            "nodes": parsed["device"].get("nodes", ""),
            "output_shapes": parsed["device"].get("output_shapes", ""),
            "checksum_matches": parse_int(parsed["device"].get("checksum_matches")),
            "shape_verified": parse_int(parsed["device"].get("shape_verified")),
            "rejections": parse_int(parsed["device"].get("rejections")),
            "rejection_guards": parsed["device"].get("rejection_guards", ""),
            "rejection_reasons": parsed["device"].get("rejection_reasons", ""),
            "status": parsed["device"].get("status", ""),
        },
        "timing": timing_report(parsed),
        "context": context_report(parsed),
        "artifacts": artifact_sizes,
        "output_guard": output_guard_result,
        "context_guard": context_guard_result,
        "prefix_cache_guard": prefix_cache_guard_result,
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
    output_guard = report["output_guard"]
    if output_guard["enabled"]:
        print(
            "output_guard: "
            f"status={output_guard['status']} tokenizer={output_guard['tokenizer']} "
            f"expect_regexes={json.dumps(output_guard['expect_regexes'])} "
            f"reject_regexes={json.dumps(output_guard['reject_regexes'])} "
            f"text={json.dumps(output_guard['text'])}"
        )
    context_guard = report["context_guard"]
    if context_guard["enabled"]:
        print(
            "context_guard: "
            f"status={context_guard['status']} "
            f"required_contexts={json.dumps(context_guard['required_contexts'])}"
        )
    prefix_cache_guard = report["prefix_cache_guard"]
    if prefix_cache_guard["enabled"]:
        print(
            "prefix_cache_guard: "
            f"status={prefix_cache_guard['status']} "
            f"required={str(prefix_cache_guard['required']).lower()}"
        )
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
    prefix_cache = report["prefix_cache"]
    print(
        "prefix_cache: "
        f"ids={prefix_cache['ids']} action={prefix_cache['actions']} "
        f"kv_hits={prefix_cache['kv_hits']} kv_nodes={prefix_cache['kv_nodes']} "
        f"gsva_rejections={prefix_cache['gsva_rejections']} "
        f"gsva_rejection_reasons={prefix_cache['gsva_rejection_reasons']} "
        f"reject_policy={prefix_cache['reject_policy']} "
        f"recompute_range_forwards={prefix_cache['recompute_range_forwards']} "
        f"reject_then_recompute={prefix_cache['reject_then_recompute']}"
    )
    gsva = report["gsva"]
    print(
        "gsva: "
        f"kv_refs={gsva['kv_refs']} reads={gsva['reads']} "
        f"writebacks={gsva['writebacks']} kv_nodes={gsva['kv_nodes']}"
    )
    gsva_timing = gsva["timing"]
    print(
        "gsva_timing: "
        f"records={gsva_timing['records']} "
        f"lookup_ms={gsva_timing['lookup_ms']} "
        f"map_read_ms={gsva_timing['map_read_ms']} "
        f"avoided_compute_ms={gsva_timing['avoided_compute_ms']}"
    )
    device = report["device"]
    print(
        "device: "
        f"records={device['records']} tensor_consumers={device['tensor_consumers']} "
        f"devices={device['devices']} backends={device['backends']} "
        f"ops={device['ops']} nodes={device['nodes']} "
        f"output_shapes={device['output_shapes']} "
        f"checksum_matches={device['checksum_matches']} "
        f"shape_verified={device['shape_verified']} "
        f"rejections={device['rejections']} "
        f"rejection_guards={device['rejection_guards']} "
        f"status={device['status']}"
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
    for label in CONTEXT_SUMMARY_PREFIXES:
        context = report["context"].get(label)
        if not context:
            continue
        print(
            "context: "
            f"label={label} "
            f"records={context['records']} "
            f"steps={context['steps']} "
            f"modes={context['modes']} "
            f"max_latency_ms={context['max_latency_ms']} "
            f"max_latency_step={context['max_latency_step']} "
            f"max_latency_node={context['max_latency_node']} "
            f"total_latency_ms={context['total_latency_ms']} "
            f"output_checksum_xor={context['output_checksum_xor']} "
            f"row_prefetch_hits={context['row_prefetch_hits']} "
            f"row_prefetch_requests={context['row_prefetch_requests']} "
            f"row_prefetch_hit_rate_milli={context['row_prefetch_hit_rate_milli']} "
            f"table_bytes_moved={context['table_bytes_moved']} "
            f"gate_weight_bytes_moved={context['gate_weight_bytes_moved']} "
            f"indices_bytes_moved={context['indices_bytes_moved']} "
            f"hidden_input_bytes={context['hidden_input_bytes']} "
            f"hidden_output_bytes={context['hidden_output_bytes']} "
            f"hidden_injection_overhead_bytes={context['hidden_injection_overhead_bytes']}"
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


def decode_output_key(report):
    lines = report["decode"]["output"]
    for line in lines:
        if line.startswith("decode_output: token_ids="):
            return line
    return "\n".join(lines)


def artifact_bytes(report, label):
    return report["artifacts"].get(label, {}).get("bytes")


def delta_text(value, baseline):
    if value is None or baseline is None:
        return "missing"
    return str(value - baseline)


def build_prefix_cache_comparison(baseline_summary, prefix_summary, mismatch_summary, args):
    output_guard = output_guard_from_args(args)
    context_guard = context_guard_from_args(args)
    baseline = build_report(baseline_summary, output_guard, context_guard)
    prefix = build_report(prefix_summary, output_guard, context_guard)
    mismatch = build_report(mismatch_summary, output_guard, context_guard)
    issues = []
    for label, report in (
        ("baseline", baseline),
        ("prefix", prefix),
        ("mismatch", mismatch),
    ):
        for issue in report["issues"]:
            issues.append(f"{label}: {issue}")
    if decode_output_key(baseline) != decode_output_key(prefix):
        issues.append("baseline/prefix decode_output mismatch")
    if prefix["prefix_cache"]["actions"] != "reuse":
        issues.append(
            f"prefix run did not reuse prefix cache action={prefix['prefix_cache']['actions']}"
        )
    if prefix["prefix_cache"]["kv_hits"] <= 0:
        issues.append(
            f"prefix run has no prefix-cache KV hits value={prefix['prefix_cache']['kv_hits']}"
        )
    if mismatch["prefix_cache"]["actions"] != "miss":
        issues.append(
            f"mismatch run did not miss prefix cache action={mismatch['prefix_cache']['actions']}"
        )
    return {
        "status": "pass" if not issues else "fail",
        "issues": issues,
        "baseline": baseline,
        "prefix": prefix,
        "mismatch": mismatch,
        "deltas": {
            "prefix_round_sum_ms": prefix["timing"]["round_sum_ms"]
            - baseline["timing"]["round_sum_ms"],
            "prefix_post_step0_avg_round_ms": round(
                prefix["timing"]["post_step0_avg_round_ms"]
                - baseline["timing"]["post_step0_avg_round_ms"],
                1,
            ),
            "prefix_range_forwards": prefix["shortpath"]["actual_range_forwards"]
            - baseline["shortpath"]["actual_range_forwards"],
            "prefix_memory_store_json_bytes": delta_text(
                artifact_bytes(prefix, "memory_store_json"),
                artifact_bytes(baseline, "memory_store_json"),
            ),
            "prefix_object_store_json_bytes": delta_text(
                artifact_bytes(prefix, "object_store_json"),
                artifact_bytes(baseline, "object_store_json"),
            ),
        },
    }


def print_prefix_cache_comparison(comparison):
    print(f"w5_prefix_cache_comparison: status={comparison['status']}")
    for label in ("baseline", "prefix", "mismatch"):
        report = comparison[label]
        prefix_cache = report["prefix_cache"]
        timing = report["timing"]
        print(
            "comparison_run: "
            f"label={label} status={report['status']} run_id={report['run_id']} "
            f"prefix_cache_ids={prefix_cache['ids']} "
            f"prefix_cache_action={prefix_cache['actions']} "
            f"prefix_cache_kv_hits={prefix_cache['kv_hits']} "
            f"round_sum_ms={timing['round_sum_ms']} "
            f"post_step0_avg_round_ms={timing['post_step0_avg_round_ms']} "
            f"range_forwards={report['shortpath']['actual_range_forwards']} "
            f"memory_store_json_bytes={artifact_bytes(report, 'memory_store_json')} "
            f"object_store_json_bytes={artifact_bytes(report, 'object_store_json')}"
        )
    deltas = comparison["deltas"]
    print(
        "comparison_delta: "
        f"prefix_round_sum_ms={deltas['prefix_round_sum_ms']} "
        f"prefix_post_step0_avg_round_ms={deltas['prefix_post_step0_avg_round_ms']} "
        f"prefix_range_forwards={deltas['prefix_range_forwards']} "
        f"prefix_memory_store_json_bytes={deltas['prefix_memory_store_json_bytes']} "
        f"prefix_object_store_json_bytes={deltas['prefix_object_store_json_bytes']}"
    )
    for issue in comparison["issues"]:
        print(f"issue: {issue}")


def main(argv):
    parser = argparse.ArgumentParser(
        description="Audit a W5 inference cluster run summary and its artifacts."
    )
    parser.add_argument("summary", type=Path, nargs="?")
    parser.add_argument(
        "--compare-prefix-cache",
        nargs=3,
        metavar=("BASELINE", "PREFIX", "MISMATCH"),
        type=Path,
        help="Compare no-prefix baseline, prefix-cache reuse, and mismatched-prefix recompute summaries.",
    )
    parser.add_argument("--json", action="store_true", dest="json_output")
    parser.add_argument(
        "--tokenizer-dir",
        type=Path,
        default=None,
        help="Tokenizer directory or tokenizer.json used to decode token_ids for output guards.",
    )
    parser.add_argument(
        "--expect-output-regex",
        action="append",
        default=[],
        help="Require tokenizer-decoded output text to match this regex. Repeatable.",
    )
    parser.add_argument(
        "--reject-output-regex",
        action="append",
        default=[],
        help="Fail if tokenizer-decoded output text matches this regex. Repeatable.",
    )
    parser.add_argument(
        "--require-context",
        action="append",
        choices=CONTEXT_SUMMARY_PREFIXES,
        default=[],
        help="Require a context summary with at least one record. Repeatable.",
    )
    parser.add_argument(
        "--require-device-gsva",
        action="store_true",
        help="Require a W5 NPU GSVA tensor consumer with checksum/shape parity and token/epoch/retire rejections.",
    )
    parser.add_argument(
        "--require-prefix-cache",
        action="store_true",
        help="Require prefix-cache reuse hit and evidence.",
    )
    args = parser.parse_args(argv)

    if args.compare_prefix_cache:
        for summary in args.compare_prefix_cache:
            if not summary.is_file():
                print(f"summary file is missing: {summary}", file=sys.stderr)
                return 2
        comparison = build_prefix_cache_comparison(
            args.compare_prefix_cache[0],
            args.compare_prefix_cache[1],
            args.compare_prefix_cache[2],
            args,
        )
        if args.json_output:
            print(json.dumps(comparison, indent=2, sort_keys=True))
        else:
            print_prefix_cache_comparison(comparison)
        return 0 if comparison["status"] == "pass" else 1

    if args.summary is None:
        parser.error("summary is required unless --compare-prefix-cache is used")
    if not args.summary.is_file():
        print(f"summary file is missing: {args.summary}", file=sys.stderr)
        return 2

    report = build_report(
        args.summary,
        output_guard_from_args(args),
        context_guard_from_args(args),
        args.require_device_gsva,
        prefix_cache_guard_from_args(args),
    )
    if args.json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_text_report(report)
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
