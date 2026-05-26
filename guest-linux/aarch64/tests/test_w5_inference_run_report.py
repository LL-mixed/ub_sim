#!/usr/bin/env python3
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "w5_inference_run_report.py"


def write_artifacts(out_dir, run_id):
    (out_dir / f"w5_memory_object_store.{run_id}.json").write_text("{}", encoding="utf-8")
    (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
    (out_dir / f"w5_object_service_store.{run_id}.bin").write_bytes(b"binary")
    registry = out_dir / f"w5_memory_registry.{run_id}"
    registry.mkdir()
    (registry / "w5_memory_shortpath_stream.txt").write_text("shortpath\n", encoding="utf-8")
    (registry / "w5_memory_shortpath_kv_stream.txt").write_text("kv\n", encoding="utf-8")


def write_tokenizer(path):
    path.mkdir()
    (path / "tokenizer.json").write_text(
        (
            '{"model":{"vocab":{",":11,"\\u0120ok":22,'
            '"_ComCallableWrapper":88950,"\\u0120Huawei":81378,'
            '"\\u0120is":374}},"decoder":{"type":"ByteLevel"}}'
        ),
        encoding="utf-8",
    )


def write_summary(path, run_dir, bad_marker="", token_ids="[11, 22]", token_pieces='", ok"'):
    lines = [
        f"summary: run_dir={run_dir}",
        (
            "summary: decode_steps_expected=2 decode_steps_observed=2 "
            "worker_timing_records=2 passed_nodes=8/8 handoff_timing_records=2 "
            "idle_timing_records=14 engram_timing_records=16 engram_context_records=2"
        ),
        f"decode_output: token_ids={token_ids}",
        f"decode_output: token_pieces={token_pieces}",
        (
            "timing_step: step=0 round_ms=100 critical_node=nodeA workers=1/8 "
            "max_worker_ms=100 avg_worker_ms=100 max_input_wait_ms=0 "
            "max_compute_window_ms=70 max_submit_ms=50 max_publish_ms=5 max_barrier_ms=0"
        ),
        (
            "timing_step: step=1 round_ms=40 critical_node=nodeA workers=1/8 "
            "max_worker_ms=40 avg_worker_ms=40 max_input_wait_ms=1 "
            "max_compute_window_ms=30 max_submit_ms=20 max_publish_ms=4 max_barrier_ms=0"
        ),
        (
            "engram_timing_step: step=0 nodes=8/8 candidate_publish_ms=0 "
            "candidate_wait_ms=0 policy_select_ms=1 decision_publish_ms=0 "
            "selected_wait_ms=0 selected_writeback_ms=2 history_state_wait_ms=0 "
            "engram_total_ms=3 max_qwen3_range_publish_ms=5 "
            "max_qwen3_range_input_wait_ms=0 bottleneck=range_pipeline bottleneck_ms=5 "
            "idle_nodes=7"
        ),
        (
            "engram_timing_step: step=1 nodes=8/8 candidate_publish_ms=0 "
            "candidate_wait_ms=0 policy_select_ms=1 decision_publish_ms=0 "
            "selected_wait_ms=0 selected_writeback_ms=2 history_state_wait_ms=0 "
            "engram_total_ms=3 max_qwen3_range_publish_ms=4 "
            "max_qwen3_range_input_wait_ms=1 bottleneck=range_pipeline bottleneck_ms=5 "
            "idle_nodes=7"
        ),
        (
            "memory_service_summary: service=lingqu_memory_service records=18 steps=2/2 "
            "stages=qwen3_memory_service_boundary_lookup_request:2,"
            "qwen3_memory_service_boundary_lookup_response:2,"
            "qwen3_w5_memory_boundary_decision:2,"
            "qwen3_w5_memory_boundary_registry_loaded:8,"
            "qwen3_w5_memory_decision_contract:8,"
            "qwen3_w5_memory_shortpath_commit:2,"
            "qwen3_w5_memory_shortpath_kv_stream_loaded:8,"
            "qwen3_w5_memory_terminal_logits_loaded:2,"
            "qwen3_w5_memory_terminal_logits_selected:2 "
            "shortpath_ids=runtime_service_catalog support_ids=boundary_registry "
            "actions=jump-to-terminal artifact_kinds=logits prefetch_ids=none "
            "prefix_cache_ids=none lookup_hits=2 hit_registry_indexes=0,7 "
            "hit_registry_steps=0,1 hit_positions=3,4"
        ),
        (
            "guest_worker_shortpath_summary: action=jump-to-terminal "
            "boundary_hits=2 terminal_selects=2 expected_hits=2 "
            "actual_range_forwards=2 actual_runtime_inputs=1 actual_runtime_outputs=0 "
            "shortpath_no_dispatch=14 shortpath_terminal_commits=14 "
            "shortpath_publish_hidden_zero=2 full_pipeline_range_forwards=16 "
            "full_pipeline_runtime_inputs=15 full_pipeline_runtime_outputs=16"
        ),
    ]
    if bad_marker:
        lines.append(bad_marker)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class W5InferenceRunReportTest(unittest.TestCase):
    def test_reports_passed_shortpath_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(summary, logs_dir)

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertIn("decode: steps=2/2 passed_nodes=8/8", result.stdout)
        self.assertIn(
            "shortpath: lookup_hits=2 action=jump-to-terminal artifact_kinds=logits "
            "boundary_hits=2 actual_range_forwards=2 actual_runtime_inputs=1 "
            "actual_runtime_outputs=0 shortpath_no_dispatch=14 "
            "shortpath_terminal_commits=14",
            result.stdout,
        )
        self.assertIn(
            "timing: steps=2 round_sum_ms=140 avg_round_ms=70.0 "
            "post_step0_avg_round_ms=40.0 compute_sum_ms=100 publish_sum_ms=9 "
            "engram_total_ms=6 engram_avg_ms=3.0",
            result.stdout,
        )
        self.assertIn("artifact: label=object_store_bin bytes=6", result.stdout)
        self.assertNotIn("issue:", result.stdout)

    def test_reports_shared_artifact_paths_from_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            shared = Path(tmp) / "shared"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            shared.mkdir()
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            memory_store = shared / "w5_memory_object_store.shared.json"
            object_store = shared / "w5_object_service_store.shared.json"
            registry = shared / "w5_memory_registry.shared"
            memory_store.write_text("{}", encoding="utf-8")
            object_store.write_text("{}", encoding="utf-8")
            object_store.with_suffix(".bin").write_bytes(b"binary")
            registry.mkdir()
            (registry / "w5_memory_shortpath_stream.txt").write_text(
                "shortpath\n", encoding="utf-8"
            )
            (registry / "w5_memory_shortpath_kv_stream.txt").write_text(
                "kv\n", encoding="utf-8"
            )
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(summary, logs_dir)
            env = os.environ.copy()
            env.update(
                {
                    "SIM_W5_MEMORY_STORE": str(memory_store),
                    "SIM_W5_MEMORY_OBJECT_STORE": str(object_store),
                    "SIM_W5_MEMORY_REGISTRY_DIR": str(registry),
                }
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=True,
                capture_output=True,
                text=True,
                env=env,
            )

        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertIn(f"artifact: label=memory_store_json bytes=2", result.stdout)
        self.assertIn(f"path={memory_store}", result.stdout)
        self.assertIn(f"path={object_store.with_suffix('.bin')}", result.stdout)
        self.assertNotIn("issue:", result.stdout)

    def test_fails_on_bad_shortpath_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(summary, logs_dir, "handoff_node: node=nodeB status=missing")

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("w5_run_report: status=fail run_id=run", result.stdout)
        self.assertIn("issue: bad marker present: status=missing", result.stdout)

    def test_output_guard_passes_expected_decoded_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            tokenizer_dir = Path(tmp) / "tokenizer"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            write_tokenizer(tokenizer_dir)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(summary, logs_dir)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--tokenizer-dir",
                    str(tokenizer_dir),
                    "--expect-output-regex",
                    "^, ok$",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertIn("output_guard: status=pass", result.stdout)
        self.assertIn('text=", ok"', result.stdout)

    def test_output_guard_fails_rejected_decoded_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            tokenizer_dir = Path(tmp) / "tokenizer"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            write_tokenizer(tokenizer_dir)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                token_ids="[88950]",
                token_pieces='"_ComCallableWrap"',
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--tokenizer-dir",
                    str(tokenizer_dir),
                    "--reject-output-regex",
                    "ComCallableWrapper",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("w5_run_report: status=fail run_id=run", result.stdout)
        self.assertIn(
            "issue: output guard: output text rejected by regex: ComCallableWrapper",
            result.stdout,
        )

    def test_output_guard_decodes_token_ids_instead_of_truncated_token_pieces(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            tokenizer_dir = Path(tmp) / "tokenizer"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            write_tokenizer(tokenizer_dir)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                token_ids="[88950]",
                token_pieces='"_ComCallableWrap"',
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--tokenizer-dir",
                    str(tokenizer_dir),
                    "--expect-output-regex",
                    "Wrapper$",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("output_guard: status=pass", result.stdout)
        self.assertIn('text="_ComCallableWrapper"', result.stdout)


if __name__ == "__main__":
    unittest.main()
