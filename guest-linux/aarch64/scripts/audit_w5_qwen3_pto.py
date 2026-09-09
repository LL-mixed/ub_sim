#!/usr/bin/env python3
"""Audit two-node W5 PTO execution and reference top-four logits from logs."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct

LOGIT_ABS_LIMIT = 0.02


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def number(record, key):
    return int(record[key], 0)


def float_bits(value):
    result = struct.unpack("<f", int(value, 0).to_bytes(4, "little"))[0]
    if not math.isfinite(result):
        raise ValueError("non-finite logit")
    return result


def compare_logits(candidate, reference, steps):
    result = []
    for name, records in (("candidate", candidate), ("reference", reference)):
        if [number(row, "step") for row in records] != list(range(steps)):
            raise ValueError(f"{name}: missing, duplicate or reordered logits steps")
    for c, r in zip(candidate, reference):
        for key in ("token", "runner_up", "full_vocab_checked", "candidate_count"):
            if c[key] != r[key]:
                raise ValueError(f"step {c['step']}: mismatched {key}")
        if number(c, "candidate_count") != 4 or number(c, "full_vocab_checked") != 151936:
            raise ValueError("requires all four candidates and Qwen3-0.6B vocabulary")
        errors = []
        for rank in range(4):
            prefix = f"candidate{rank}_"
            for key in ("token", "text_checksum", "piece_bytes", "piece_word0", "piece_word1"):
                if c[prefix + key] != r[prefix + key]:
                    raise ValueError(f"step {c['step']}: candidate {rank} differs: {key}")
            error = abs(float_bits(c[prefix + "logit_bits"]) - float_bits(r[prefix + "logit_bits"]))
            if error > LOGIT_ABS_LIMIT:
                raise ValueError(f"step {c['step']}: logit error {error} exceeds {LOGIT_ABS_LIMIT}")
            errors.append(error)
        result.append({"step": number(c, "step"), "token": number(c, "token"),
                       "runner_up": number(c, "runner_up"), "top4_max_abs_error": max(errors)})
    return result


def read_evidence(path, markers):
    digest = hashlib.sha256()
    found = {marker: [] for marker in markers}
    with path.open("rb") as log:
        for raw in log:
            digest.update(raw)
            line = raw.decode("utf-8", errors="replace").strip()
            if "[w4_guest] fail" in line or "Kernel panic" in line:
                raise ValueError(f"failure in {path}: {line}")
            for marker in markers:
                if marker in line:
                    found[marker].append(fields(line))
    return found, digest.hexdigest()


def expected_operator_counts(node, tokens, past):
    layers, heads = 14, 16
    terminal = int(node == 2)
    return {"copy": layers * (3 if past else 1) + (tokens if node == 1 else 0) + 1,
            "rms_norm": layers * 4 + terminal,
            "linear": layers * (7 + 2 * heads * tokens) + terminal,
            "rope": layers * 2 * tokens, "attention": layers * heads,
            "residual": layers * 2, "swiglu": layers, "softmax": layers * heads * tokens,
            "embedding": int(node == 1), "decoder_layer": layers, "terminal_logits": terminal}


def audit_operator_counts(records, node, steps, prompt_tokens):
    if len(records) != steps:
        raise ValueError(f"node{node}: missing or duplicate operator coverage")
    totals = {key: 0 for key in expected_operator_counts(node, 1, 0)}
    for step, row in enumerate(records):
        tokens = prompt_tokens if step == 0 else 1
        past = 0 if step == 0 else prompt_tokens + step - 1
        expected = expected_operator_counts(node, tokens, past)
        geometry = {"first": (node - 1) * 14, "end": node * 14,
                    "tokens": tokens, "past": past, "kernel_complete": 1}
        if any(number(row, key) != value for key, value in {**expected, **geometry}.items()):
            raise ValueError(f"node{node}: incorrect operator coverage at step {step}")
        for key in totals:
            totals[key] += number(row, key)
    return totals


def audit(candidate, reference, steps, prompt_tokens):
    report = {"status": "pass", "scope": "two-node functional and top-four logit audit",
              "logit_abs_limit": LOGIT_ABS_LIMIT, "nodes": [], "log_sha256": {},
              "not_covered": ["full-vocabulary elementwise errors", "per-layer hidden errors",
                              "timeout recovery", "performance"]}
    observations = "stage model_terminal_logits_observation "
    for node, label in enumerate(("nodeA", "nodeB"), 1):
        markers = ["stage w5_qwen3_pto_range_complete ", "[w4_guest] pass",
                   "stage w5_pto_ub_gm_hidden_publish ", "stage model_range_kv_state_resolve ",
                   "stage uapi_model_range_runtime_forward", observations,
                   "stage model_range_kv_state_publish "]
        path = candidate / f"{label}_guest.log"
        guest, digest = read_evidence(path, markers)
        report["log_sha256"][str(path)] = digest
        ranges = guest[markers[0]]
        if len(guest[markers[1]]) != steps or len(guest[markers[2]]) != steps:
            raise ValueError(f"{label}: incomplete pass or in-place hidden publications")
        if guest[markers[4]] or [number(r, "step") for r in ranges] != list(range(steps)):
            raise ValueError(f"{label}: reference fallback or incomplete ranges")
        expected_range = "[0,14)" if node == 1 else "[14,28)"
        for step, row in enumerate(ranges):
            tokens = prompt_tokens if step == 0 else 1
            past = 0 if step == 0 else prompt_tokens + step - 1
            expected = {"node": node, "step": step, "tokens": tokens, "past": past,
                        "callable": 3, "hidden_bytes": tokens * 1024 * 2,
                        "kv_bytes": 14 * (40 + 2 * (past + tokens) * 1024 * 4)}
            if any(number(row, k) != v for k, v in expected.items()) or row["layers"] != expected_range:
                raise ValueError(f"{label}: incorrect range geometry at step {step}")
            if row["status"] != "ok" or row["numerical_backend"] != "simpler_pto":
                raise ValueError(f"{label}: unsuccessful PTO computation")
        for row in guest[markers[2]]:
            if row["publish_mode"] != "in_place" or row["backing"] != "obmm_shmem" or row["status"] != "ok":
                raise ValueError(f"{label}: invalid hidden publication")
        kv = guest[markers[3]]
        if [number(row, "kv_step") for row in kv] != list(range(steps - 1)):
            raise ValueError(f"{label}: incomplete previous KV resolution")
        if any(row["backing"] != "obmm_shmem" or row["status"] != "ok" for row in kv):
            raise ValueError(f"{label}: previous KV did not use OBMM backing")
        publications = guest[markers[6]]
        if [number(row, "step") for row in publications] != list(range(steps)):
            raise ValueError(f"{label}: incomplete or duplicated KV publications")
        for produced, published in zip(ranges, publications):
            if (published["payload_mode"] != "in_place" or
                number(published, "publication_copy_bytes") != 0 or
                number(published, "offset") != number(produced, "kv_offset") or
                number(published, "kv_bytes") != number(produced, "kv_bytes") or
                published["backing"] != "obmm_shmem" or published["status"] != "ok"):
                raise ValueError(f"{label}: KV publication changed the PTO output backing")
        for resolved, published in zip(kv, publications):
            for key in ("offset", "kv_bytes", "kv_checksum", "key_hash", "version"):
                if number(resolved, key) != number(published, key):
                    raise ValueError(f"{label}: next decode resolved different KV {key}")
        path = candidate / f"{label}_qemu.log"
        qemu, digest = read_evidence(path, ["QEMU_UB_GM_UNBIND ", "qwen3-pto-range:",
                                           "QWEN3_PTO_OPERATOR_COUNTS "])
        report["log_sha256"][str(path)] = digest
        completions = qemu["QEMU_UB_GM_UNBIND "]
        model = qemu["qwen3-pto-range:"]
        expected_ids = [0x5157000000000000 | ((node - 1) << 32) | (step + 1) for step in range(steps)]
        for rows in (completions, model):
            if [number(row, "request") for row in rows] != expected_ids:
                raise ValueError(f"{label}: incomplete or duplicated request completions")
        for row in completions:
            if (row["reason"] != "completion_success" or number(row, "bindings") != 6 or
                number(row, "fences") != node + 1 or number(row, "load_bytes") <= 0 or
                number(row, "store_bytes") <= 0 or number(row, "segment_payload_staging_bytes") != 0):
                raise ValueError(f"{label}: invalid UB_GM completion")
        if any(row["status"] != "pass" or number(row, "shared_payload_staging_bytes") != 0 for row in model):
            raise ValueError(f"{label}: failed model invocation or shared staging")
        report["nodes"].append({"node": node, "range": expected_range, "steps": steps,
                                "operator_invocations": audit_operator_counts(
                                    qemu["QWEN3_PTO_OPERATOR_COUNTS "], node, steps, prompt_tokens),
                                "previous_kv_resolutions": len(kv),
                                "kv_in_place_publications": len(publications),
                                "kv_publication_copy_bytes": 0,
                                **{key: sum(number(r, key) for r in completions)
                                   for key in ("load_bytes", "store_bytes", "fences")}})
        path = reference / f"{label}_guest.log"
        baseline, digest = read_evidence(path, ["[w4_guest] pass", observations])
        report["log_sha256"][str(path)] = digest
        if len(baseline["[w4_guest] pass"]) != steps:
            raise ValueError(f"{label}: incomplete reference run")
        if node == 2:
            report["logits"] = compare_logits(guest[observations], baseline[observations], steps)
    report["top4_max_abs_error"] = max(row["top4_max_abs_error"] for row in report["logits"])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-logs", type=Path, required=True)
    parser.add_argument("--reference-logs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--steps", type=int, default=8)
    parser.add_argument("--prompt-tokens", type=int, default=3)
    args = parser.parse_args()
    if args.steps < 1 or args.prompt_tokens < 1:
        parser.error("steps and prompt tokens must be positive")
    try:
        report = audit(args.candidate_logs, args.reference_logs, args.steps, args.prompt_tokens)
    except (ValueError, KeyError, OSError) as error:
        report = {"status": "fail", "error": str(error)}
    with args.output.open("x") as output:
        output.write(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
