#!/usr/bin/env python3
"""Run W5 Qwen3 two-node PP with model callable 3 and retain run evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
GUEST = ROOT / "guest-linux/aarch64"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--steps", type=int, default=8)
    parser.add_argument("--tokens", default="81378,37585,374")
    parser.add_argument("--sim-cli", type=Path, default=ROOT / "target/release/sim-cli")
    parser.add_argument("--print-plan", action="store_true")
    parser.add_argument("--numerical-trace", action="store_true",
                        help="retain per-layer hidden/KV and full-logit snapshots; no performance claims")
    args = parser.parse_args()
    tokens = [int(token) for token in args.tokens.split(",")]
    if not 1 <= len(tokens) <= 1024 or any(token < 0 for token in tokens):
        parser.error("tokens must contain 1..1024 non-negative token IDs")
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", args.run_id) or not 1 <= args.steps <= 128:
        parser.error("run-id must be a safe unique name; steps must be 1..128")
    evidence = ROOT / "out/w5-qwen3-pto" / args.run_id
    plan = {"nodes": 2, "steps": args.steps, "tokens": tokens, "callable": 3, "run_id": args.run_id,
            "manifest": str(args.manifest.resolve()), "weights": str(args.weights.resolve()),
            "scenario": str(args.scenario.resolve()), "evidence": str(evidence),
            "numerical_trace": args.numerical_trace,
            "scope": "W5 model functional run; numerical/reference audit is separate"}
    if args.print_plan:
        print(json.dumps(plan, indent=2))
        return 0
    query = [str(args.sim_cli.resolve()), "lingqu-shmem-pto-e2e", "--fingerprint-manifest", plan["manifest"]]
    fingerprint = json.loads(subprocess.check_output(query, text=True))
    if fingerprint["callable_id"] != 3:
        raise ValueError("requires a Qwen3 model-range callable 3 manifest")
    config = json.loads((args.weights / "config.json").read_text())
    if any(token >= config["vocab_size"] for token in tokens):
        raise ValueError("token outside configured vocabulary")
    evidence.mkdir(parents=True, exist_ok=False)
    (evidence / "plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    (evidence / "fingerprint-before.json").write_text(json.dumps(fingerprint, indent=2) + "\n")
    values = {
        "RUN_ID": args.run_id, "SIM_W5_CLUSTER_NODE_COUNT": "2",
        "SIM_UAPI_W5_PROFILE": "qwen3_0_6b_decode", "SIM_W5_QWEN3_PTO": "1",
        "SIM_W5_PTO_UB_GM_PROBE": "0", "SIM_W5_PTO_UB_GM_PUBLISH_OUTPUT": "0",
        "SIM_W5_PTO_UB_GM_DISABLE_EXPERIMENTAL_GSVA": "1",
        "SIM_LINGQU_SHMEM_PTO_ENABLE": "1", "SIM_QWEN3_ENGRAM_ENABLE": "0",
        "SIMPLER_QWEN3_PTO_RANGE_MANIFEST": plan["manifest"],
        "SIM_W5_PTO_UB_GM_ARTIFACT_FINGERPRINT": fingerprint["artifact_fingerprint_hex"],
        "SIM_QWEN3_DENSE_WEIGHTS_PATH": plan["weights"],
        "SIM_UAPI_SCENARIO_CONFIG": plan["scenario"], "SIM_QWEN3_GUEST_DECODE_STEPS": str(args.steps),
        "SIM_LLM_INFER_PROMPT_TOKEN_IDS": args.tokens,
        "SIM_QWEN3_DENSE_TP_NODES": "2", "SIM_QWEN3_DENSE_PREFILL_TOKENS": str(len(tokens)),
        "SIM_QWEN3_DENSE_DECODE_TOKENS": "1",
        "SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES": str(len(tokens) * config["hidden_size"] * 2),
        "SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES": str(config["hidden_size"] * 2),
    }
    if args.numerical_trace:
        trace = evidence / "numerical-trace"
        trace.mkdir(exist_ok=False)
        values["SIM_QWEN3_PTO_TRACE_DIR"] = str(trace)
    else:
        values["SIM_QWEN3_PTO_TRACE_DIR"] = ""
    (evidence / "runtime.env").write_text("".join(f"{key}={json.dumps(value)}\n" for key, value in sorted(values.items())))
    environment = os.environ.copy()
    environment.update(values)
    with (evidence / "runner.log").open("x") as log:
        result = subprocess.run(["zsh", str(GUEST / "scripts/run_llm_infer_eight_node_guest.sh")],
                                cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT)
    after = json.loads(subprocess.check_output(query, text=True))
    (evidence / "fingerprint-after.json").write_text(json.dumps(after, indent=2) + "\n")
    status = "pass" if result.returncode == 0 and fingerprint == after else "fail"
    report = {"status": status, "runner_exit_code": result.returncode, "fingerprint_unchanged": fingerprint == after,
              "scope": plan["scope"], "logs": str(GUEST / "logs" / (args.run_id + "_headless8"))}
    (evidence / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
