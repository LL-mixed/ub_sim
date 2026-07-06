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


def write_summary(
    path,
    run_dir,
    bad_marker="",
    token_ids="[11, 22]",
    token_pieces='", ok"',
    context_lines=(),
    device_lines=(),
):
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
    lines.extend(context_lines)
    lines.extend(device_lines)
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

    def test_requires_device_gsva_tensor_consumer(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                device_lines=(
                    (
                        "w5_device_summary: records=4 tensor_consumers=1 devices=npu "
                        "backends=gsva ops=vector_add_u32 nodes=0 output_shapes=16 "
                        "checksum_matches=1 shape_verified=1 rejections=3 "
                        "rejection_guards=token,epoch,retire "
                        "rejection_reasons=token_denied,stale_epoch,segment_retired "
                        "status=ok"
                    ),
                ),
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary), "--require-device-gsva"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "device: records=4 tensor_consumers=1 devices=npu backends=gsva "
            "ops=vector_add_u32 nodes=0 output_shapes=16 checksum_matches=1 "
            "shape_verified=1 rejections=3 rejection_guards=token,epoch,retire "
            "status=ok",
            result.stdout,
        )
        self.assertNotIn("issue:", result.stdout)

    def test_require_device_gsva_rejects_missing_guards(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                device_lines=(
                    (
                        "w5_device_summary: records=2 tensor_consumers=1 devices=npu "
                        "backends=gsva ops=vector_add_u32 nodes=0 output_shapes=16 "
                        "checksum_matches=1 shape_verified=1 rejections=1 "
                        "rejection_guards=token rejection_reasons=token_denied "
                        "status=ok"
                    ),
                ),
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary), "--require-device-gsva"],
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "issue: device GSVA rejection guards missing value=epoch,retire",
            result.stdout,
        )

    def test_reports_passed_prefix_cache_only_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            registry = out_dir / f"w5_memory_registry.{run_id}"
            (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                "prefix-kv\n", encoding="utf-8"
            )
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            summary.write_text(
                "\n".join(
                    [
                        f"summary: run_dir={logs_dir}",
                        (
                            "summary: decode_steps_expected=2 decode_steps_observed=2 "
                            "worker_timing_records=16 passed_nodes=8/8 "
                            "handoff_timing_records=0 idle_timing_records=0 "
                            "engram_timing_records=0 engram_context_records=0 "
                            "paper_engram_context_records=0 "
                            "fused_simt_context_records=0 "
                            "fused_simt_vendor_context_records=0"
                        ),
                        "decode_output: token_ids=[81378, 374]",
                        (
                            "memory_service_summary: service=lingqu_memory_service "
                            "records=11 steps=2/2 "
                            "stages=qwen3_w5_memory_decision_contract:8,"
                            "qwen3_w5_memory_gsva_kv_loaded:1,"
                            "qwen3_w5_memory_prefix_cache_kv_loaded:1,"
                            "qwen3_w5_memory_prefix_cache_kv_stream_loaded:1 "
                            "shortpath_ids=none support_ids=none actions=none "
                            "artifact_kinds=none prefetch_ids=none "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-test "
                            "prefix_cache_actions=reuse prefix_cache_kv_hits=1 "
                            "prefix_cache_kv_nodes=1 prefix_cache_matched_tokens=3,3 "
                            "prefix_cache_suffix_replay_tokens=2 "
                            "prefix_cache_suffix_replay_steps=0,1 "
                            "prefix_cache_gsva_rejections=0 "
                            "prefix_cache_gsva_rejection_reasons=none "
                            "gsva_kv_refs=1 gsva_reads=1 "
                            "gsva_writebacks=0 gsva_kv_nodes=1 lookup_hits=0 "
                            "hit_registry_indexes=none hit_registry_steps=none "
                            "hit_positions=none"
                        ),
                        (
                            "memory_service_request: request_id=req-shared-prefix "
                            "records=4 steps=0,1 "
                            "stages=qwen3_w5_memory_gsva_kv_loaded:1,"
                            "qwen3_w5_memory_prefix_cache_kv_loaded:1,"
                            "qwen3_w5_memory_prefix_cache_suffix_replay_token:2 "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-test "
                            "prefix_cache_actions=reuse prefix_cache_kv_hits=1 "
                            "prefix_cache_kv_nodes=1 "
                            "prefix_cache_gsva_rejections=0 "
                            "prefix_cache_gsva_rejection_reasons=none "
                            "gsva_kv_refs=1 gsva_reads=1 "
                            "gsva_writebacks=0 gsva_kv_nodes=1 "
                            "prefix_cache_matched_tokens=3 "
                            "prefix_cache_suffix_replay_tokens=2 "
                            "prefix_cache_suffix_replay_steps=0,1"
                        ),
                        "gsva_timing: records=1 lookup_ms=2 map_read_ms=1 avoided_compute_ms=0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertIn("artifact: label=prefix_cache_kv_stream bytes=10", result.stdout)
        self.assertIn("gsva: kv_refs=1 reads=1 writebacks=0 kv_nodes=1", result.stdout)
        self.assertIn(
            "serving_request: request_id=req-shared-prefix records=4 steps=0,1",
            result.stdout,
        )
        self.assertIn("prefix_cache_kv_hits=1", result.stdout)
        self.assertIn(
            "gsva_timing: records=1 lookup_ms=2 map_read_ms=1 avoided_compute_ms=0",
            result.stdout,
        )
        self.assertNotIn("issue:", result.stdout)

    def test_reports_passed_prefix_cache_miss_recompute_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            registry = out_dir / f"w5_memory_registry.{run_id}"
            (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                "prefix-kv\n",
                encoding="utf-8",
            )
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            summary.write_text(
                "\n".join(
                    [
                        f"summary: run_dir={logs_dir}",
                        (
                            "summary: decode_steps_expected=2 decode_steps_observed=2 "
                            "worker_timing_records=16 passed_nodes=8/8 "
                            "handoff_timing_records=0 idle_timing_records=0 "
                            "engram_timing_records=0 engram_context_records=0 "
                            "paper_engram_context_records=0 "
                            "fused_simt_context_records=0 "
                            "fused_simt_vendor_context_records=0"
                        ),
                        "decode_output: token_ids=[81378, 374]",
                        (
                            "memory_service_summary: service=lingqu_memory_service "
                            "records=8 steps=2/2 "
                            "stages=qwen3_w5_memory_decision_contract:8 "
                            "shortpath_ids=none support_ids=none actions=none "
                            "artifact_kinds=none prefetch_ids=none "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-miss "
                            "prefix_cache_actions=miss prefix_cache_kv_hits=0 "
                            "prefix_cache_kv_nodes=none lookup_hits=0 "
                            "hit_registry_indexes=none hit_registry_steps=none "
                            "hit_positions=none"
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertNotIn("issue:", result.stdout)

    def test_reports_prefix_cache_guard_when_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            registry = out_dir / f"w5_memory_registry.{run_id}"
            (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                "prefix-kv\n",
                encoding="utf-8",
            )
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            summary.write_text(
                "\n".join(
                    [
                        f"summary: run_dir={logs_dir}",
                        (
                            "summary: decode_steps_expected=2 decode_steps_observed=2 "
                            "worker_timing_records=16 passed_nodes=8/8 "
                            "handoff_timing_records=8 idle_timing_records=0 "
                            "engram_timing_records=0 engram_context_records=0 "
                            "paper_engram_context_records=0 "
                            "fused_simt_context_records=0 "
                            "fused_simt_vendor_context_records=0"
                        ),
                        "decode_output: token_ids=[81378, 374]",
                        (
                            "memory_service_summary: service=lingqu_memory_service "
                            "records=11 steps=2/2 "
                            "stages=qwen3_w5_memory_decision_contract:8,"
                            "qwen3_w5_memory_gsva_kv_loaded:1,"
                            "qwen3_w5_memory_prefix_cache_kv_loaded:1,"
                            "qwen3_w5_memory_prefix_cache_kv_stream_loaded:1 "
                            "shortpath_ids=none support_ids=none actions=none "
                            "artifact_kinds=none prefetch_ids=none "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-test "
                            "prefix_cache_actions=reuse prefix_cache_kv_hits=1 "
                            "prefix_cache_kv_nodes=1 prefix_cache_matched_tokens=3,3 "
                            "prefix_cache_suffix_replay_tokens=2 "
                            "prefix_cache_suffix_replay_steps=0,1 "
                            "prefix_cache_gsva_rejections=0 "
                            "prefix_cache_gsva_rejection_reasons=none "
                            "gsva_kv_refs=1 gsva_reads=1 "
                            "gsva_writebacks=0 gsva_kv_nodes=1 lookup_hits=0 "
                            "hit_registry_indexes=none hit_registry_steps=none "
                            "hit_positions=none"
                        ),
                        "gsva_timing: records=1 lookup_ms=2 map_read_ms=1 avoided_compute_ms=0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--require-prefix-cache",
                    "--expect-prefix-cache-matched-tokens",
                    "3",
                    "--expect-prefix-cache-suffix-replay-tokens",
                    "2",
                ],
                check=True,
                capture_output=True,
                text=True,
                env={**os.environ, "SIM_W5_TEST_REQUIRE_PREFIX_CACHE": "1"},
            )
            mismatch = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--require-prefix-cache",
                    "--expect-prefix-cache-matched-tokens",
                    "4",
                    "--expect-prefix-cache-suffix-replay-tokens",
                    "2",
                ],
                check=False,
                capture_output=True,
                text=True,
                env={**os.environ, "SIM_W5_TEST_REQUIRE_PREFIX_CACHE": "1"},
            )

        self.assertIn("prefix_cache_guard: status=pass required=true", result.stdout)
        self.assertIn("matched_tokens=3", result.stdout)
        self.assertIn("suffix_replay_tokens=2", result.stdout)
        self.assertIn("effective_generated_steps=0", result.stdout)
        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertNotIn("issue:", result.stdout)
        self.assertEqual(mismatch.returncode, 1)
        self.assertIn("prefix-cache matched token mismatch expected=4 actual=3,3", mismatch.stdout)

    def test_rejects_prefix_cache_guard_when_prefix_cache_miss(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            summary.write_text(
                "\n".join(
                    [
                        f"summary: run_dir={logs_dir}",
                        (
                            "summary: decode_steps_expected=2 decode_steps_observed=2 "
                            "worker_timing_records=16 passed_nodes=8/8 "
                            "handoff_timing_records=0 idle_timing_records=0 "
                            "engram_timing_records=0 engram_context_records=0 "
                            "paper_engram_context_records=0 "
                            "fused_simt_context_records=0 "
                            "fused_simt_vendor_context_records=0"
                        ),
                        "decode_output: token_ids=[81378, 374]",
                        (
                            "memory_service_summary: service=lingqu_memory_service "
                            "records=8 steps=2/2 "
                            "stages=qwen3_w5_memory_decision_contract:8 "
                            "shortpath_ids=none support_ids=none actions=none "
                            "artifact_kinds=none prefetch_ids=none "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-miss "
                            "prefix_cache_actions=miss prefix_cache_kv_hits=0 "
                            "prefix_cache_kv_nodes=none lookup_hits=0 "
                            "hit_registry_indexes=none hit_registry_steps=none "
                            "hit_positions=none"
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary), "--require-prefix-cache"],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "prefix-cache guard: required prefix-cache action mismatch: miss",
            result.stdout,
        )

    def test_reports_passed_prefix_cache_gsva_stale_reject_then_recompute_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            registry = out_dir / f"w5_memory_registry.{run_id}"
            (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                "stale-prefix-kv\n", encoding="utf-8"
            )
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            summary.write_text(
                "\n".join(
                    [
                        f"summary: run_dir={logs_dir}",
                        (
                            "summary: decode_steps_expected=2 decode_steps_observed=2 "
                            "worker_timing_records=16 passed_nodes=8/8 "
                            "handoff_timing_records=8 idle_timing_records=0 "
                            "engram_timing_records=0 engram_context_records=0 "
                            "paper_engram_context_records=0 "
                            "fused_simt_context_records=0 "
                            "fused_simt_vendor_context_records=0"
                        ),
                        "decode_output: token_ids=[81378, 374]",
                        (
                            "memory_service_summary: service=lingqu_memory_service "
                            "records=10 steps=2/2 "
                            "stages=qwen3_w5_memory_decision_contract:8,"
                            "qwen3_w5_memory_prefix_cache_gsva_rejected:1,"
                            "qwen3_w5_memory_prefix_cache_kv_stream_loaded:1 "
                            "shortpath_ids=none support_ids=none actions=none "
                            "artifact_kinds=none prefetch_ids=none "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-stale "
                            "prefix_cache_actions=reuse prefix_cache_kv_hits=0 "
                            "prefix_cache_kv_nodes=none prefix_cache_gsva_rejections=1 "
                            "prefix_cache_gsva_rejection_reasons=epoch_mismatch "
                            "gsva_kv_refs=0 gsva_reads=0 gsva_writebacks=0 "
                            "gsva_kv_nodes=none lookup_hits=0 "
                            "hit_registry_indexes=none hit_registry_steps=none "
                            "hit_positions=none "
                            "prefix_cache_reject_policy=cache_reject_then_recompute "
                            "prefix_cache_recompute_range_forwards=16 "
                            "prefix_cache_reject_then_recompute=1"
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("w5_run_report: status=pass run_id=run", result.stdout)
        self.assertIn(
            "prefix_cache: ids=prefix-cache-reuse/runtime-stale action=reuse "
            "kv_hits=0 kv_nodes=none gsva_rejections=1 "
            "gsva_rejection_reasons=epoch_mismatch "
            "reject_policy=cache_reject_then_recompute "
            "recompute_range_forwards=16 reject_then_recompute=1",
            result.stdout,
        )
        self.assertNotIn("issue:", result.stdout)

    def test_rejects_prefix_cache_gsva_stale_without_recompute_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            registry = out_dir / f"w5_memory_registry.{run_id}"
            (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                "stale-prefix-kv\n", encoding="utf-8"
            )
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            summary.write_text(
                "\n".join(
                    [
                        f"summary: run_dir={logs_dir}",
                        (
                            "summary: decode_steps_expected=2 decode_steps_observed=2 "
                            "worker_timing_records=16 passed_nodes=8/8 "
                            "handoff_timing_records=8 idle_timing_records=0 "
                            "engram_timing_records=0 engram_context_records=0 "
                            "paper_engram_context_records=0 "
                            "fused_simt_context_records=0 "
                            "fused_simt_vendor_context_records=0"
                        ),
                        "decode_output: token_ids=[81378, 374]",
                        (
                            "memory_service_summary: service=lingqu_memory_service "
                            "records=10 steps=2/2 "
                            "stages=qwen3_w5_memory_decision_contract:8,"
                            "qwen3_w5_memory_prefix_cache_gsva_rejected:1,"
                            "qwen3_w5_memory_prefix_cache_kv_stream_loaded:1 "
                            "shortpath_ids=none support_ids=none actions=none "
                            "artifact_kinds=none prefetch_ids=none "
                            "prefix_cache_ids=prefix-cache-reuse/runtime-stale "
                            "prefix_cache_actions=reuse prefix_cache_kv_hits=0 "
                            "prefix_cache_kv_nodes=none prefix_cache_gsva_rejections=1 "
                            "prefix_cache_gsva_rejection_reasons=epoch_mismatch "
                            "gsva_kv_refs=0 gsva_reads=0 gsva_writebacks=0 "
                            "gsva_kv_nodes=none lookup_hits=0 "
                            "hit_registry_indexes=none hit_registry_steps=none "
                            "hit_positions=none "
                            "prefix_cache_reject_policy=cache_reject_then_recompute "
                            "prefix_cache_recompute_range_forwards=0 "
                            "prefix_cache_reject_then_recompute=0"
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(summary)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(1, result.returncode)
        self.assertIn(
            "issue: GSVA prefix-cache rejection missing range-forward recompute evidence",
            result.stdout,
        )
        self.assertIn(
            "issue: stale GSVA prefix-cache rejection missing reject-then-recompute marker",
            result.stdout,
        )

    def test_compares_prefix_cache_baseline_reuse_and_miss(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")

            def write_case(run_id, memory_line, round0, round1):
                write_artifacts(out_dir, run_id)
                registry = out_dir / f"w5_memory_registry.{run_id}"
                if "prefix_cache_actions=reuse" in memory_line:
                    (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                        "prefix-kv\n", encoding="utf-8"
                    )
                summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
                lines = [
                    f"summary: run_dir={logs_dir}",
                    (
                        "summary: decode_steps_expected=2 decode_steps_observed=2 "
                        "worker_timing_records=16 passed_nodes=8/8 "
                        "handoff_timing_records=0 idle_timing_records=0 "
                        "engram_timing_records=0 engram_context_records=0 "
                        "paper_engram_context_records=0 fused_simt_context_records=0 "
                        "fused_simt_vendor_context_records=0"
                    ),
                    "decode_output: token_ids=[81378, 374]",
                    (
                        f"timing_step: step=0 nodes=8/8 round_ms={round0} "
                        "max_compute_window_ms=10 max_publish_ms=1 max_barrier_ms=0"
                    ),
                    (
                        f"timing_step: step=1 nodes=8/8 round_ms={round1} "
                        "max_compute_window_ms=10 max_publish_ms=1 max_barrier_ms=0"
                    ),
                ]
                if memory_line:
                    lines.append(memory_line)
                summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
                return summary

            baseline = write_case("baseline", "", 90, 80)
            prefix = write_case(
                "prefix",
                (
                    "memory_service_summary: service=lingqu_memory_service "
                    "records=10 steps=2/2 "
                    "stages=qwen3_w5_memory_decision_contract:8,"
                    "qwen3_w5_memory_prefix_cache_kv_loaded:1 "
                    "shortpath_ids=none support_ids=none actions=none "
                    "artifact_kinds=none prefetch_ids=none "
                    "prefix_cache_ids=prefix-cache-reuse/runtime-test "
                    "prefix_cache_actions=reuse prefix_cache_kv_hits=1 "
                    "prefix_cache_kv_nodes=1 lookup_hits=0 "
                    "hit_registry_indexes=none hit_registry_steps=none hit_positions=none"
                ),
                70,
                60,
            )
            mismatch = write_case(
                "mismatch",
                (
                    "memory_service_summary: service=lingqu_memory_service "
                    "records=8 steps=2/2 "
                    "stages=qwen3_w5_memory_decision_contract:8 "
                    "shortpath_ids=none support_ids=none actions=none "
                    "artifact_kinds=none prefetch_ids=none "
                    "prefix_cache_ids=prefix-cache-reuse/runtime-miss "
                    "prefix_cache_actions=miss prefix_cache_kv_hits=0 "
                    "prefix_cache_kv_nodes=none lookup_hits=0 "
                    "hit_registry_indexes=none hit_registry_steps=none hit_positions=none"
                ),
                92,
                82,
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compare-prefix-cache",
                    str(baseline),
                    str(prefix),
                    str(mismatch),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("w5_prefix_cache_comparison: status=pass", result.stdout)
        self.assertIn(
            "comparison_run: label=prefix status=pass run_id=prefix "
            "prefix_cache_ids=prefix-cache-reuse/runtime-test "
            "prefix_cache_action=reuse prefix_cache_kv_hits=1",
            result.stdout,
        )
        self.assertIn("comparison_delta: prefix_round_sum_ms=-40", result.stdout)
        self.assertNotIn("issue:", result.stdout)

    def test_compares_prefix_cache_benefit(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")

            def write_case(
                run_id,
                memory_line,
                round0,
                round1,
                worker_records,
                idle_records,
                range_forwards,
                runtime_inputs,
                runtime_outputs,
                gsva_line="",
                token_ids="[81378, 374]",
            ):
                write_artifacts(out_dir, run_id)
                registry = out_dir / f"w5_memory_registry.{run_id}"
                if "prefix_cache_actions=reuse" in memory_line:
                    (registry / "w5_memory_prefix_cache_kv_stream.txt").write_text(
                        "prefix-kv\n", encoding="utf-8"
                    )
                summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
                lines = [
                    f"summary: run_dir={logs_dir}",
                    (
                        "summary: decode_steps_expected=2 decode_steps_observed=2 "
                        f"worker_timing_records={worker_records} passed_nodes=8/8 "
                        "handoff_timing_records=0 "
                        f"idle_timing_records={idle_records} "
                        "engram_timing_records=0 engram_context_records=0 "
                        "paper_engram_context_records=0 fused_simt_context_records=0 "
                        "fused_simt_vendor_context_records=0"
                    ),
                    f"decode_output: token_ids={token_ids}",
                    (
                        f"timing_step: step=0 nodes=8/8 round_ms={round0} "
                        "max_compute_window_ms=10 max_publish_ms=1 max_barrier_ms=0"
                    ),
                    (
                        f"timing_step: step=1 nodes=8/8 round_ms={round1} "
                        "max_compute_window_ms=10 max_publish_ms=1 max_barrier_ms=0"
                    ),
                ]
                if range_forwards is not None:
                    lines.append(
                        (
                            "guest_worker_shortpath_summary: action=jump-to-terminal "
                            "boundary_hits=2 terminal_selects=2 expected_hits=2 "
                            f"actual_range_forwards={range_forwards} "
                            f"actual_runtime_inputs={runtime_inputs} "
                            f"actual_runtime_outputs={runtime_outputs} "
                            f"shortpath_no_dispatch={idle_records} "
                            f"shortpath_terminal_commits={idle_records} "
                            "shortpath_publish_hidden_zero=2 "
                            "full_pipeline_range_forwards=16 "
                            "full_pipeline_runtime_inputs=15 "
                            "full_pipeline_runtime_outputs=16"
                        )
                    )
                if memory_line:
                    lines.append(memory_line)
                if gsva_line:
                    lines.append(gsva_line)
                summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
                return summary

            baseline = write_case("baseline", "", 90, 80, 16, 0, None, None, None)
            prefix = write_case(
                "prefix",
                (
                    "memory_service_summary: service=lingqu_memory_service "
                    "records=10 steps=2/2 "
                    "stages=qwen3_w5_memory_decision_contract:8,"
                    "qwen3_w5_memory_prefix_cache_kv_loaded:1 "
                    "shortpath_ids=runtime_service_catalog support_ids=boundary_registry "
                    "actions=jump-to-terminal artifact_kinds=logits prefetch_ids=none "
                    "prefix_cache_ids=prefix-cache-reuse/runtime-test "
                    "prefix_cache_actions=reuse prefix_cache_kv_hits=1 "
                    "prefix_cache_kv_nodes=1 prefix_cache_gsva_rejections=0 "
                    "gsva_kv_refs=5 gsva_reads=1 gsva_writebacks=4 "
                    "gsva_kv_nodes=1 lookup_hits=2 "
                    "hit_registry_indexes=none hit_registry_steps=none hit_positions=none"
                ),
                30,
                40,
                2,
                14,
                2,
                1,
                0,
                "gsva_timing: records=1 lookup_ms=3 map_read_ms=1 avoided_compute_ms=0",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compare-prefix-cache-benefit",
                    str(baseline),
                    str(prefix),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertIn("w5_prefix_cache_benefit: status=pass", result.stdout)
            self.assertIn(
                "benefit_delta: metric=round_sum_ms baseline=170 prefix=70 "
                "delta=-100 reduction_pct=58.8 speedup=2.43",
                result.stdout,
            )
            self.assertIn(
                "benefit_delta: metric=range_forwards baseline=16 prefix=2 "
                "delta=-14 reduction_pct=87.5 speedup=8.0",
                result.stdout,
            )
            self.assertIn(
                "benefit_gsva: prefix_cache_kv_hits=1 gsva_reads=1 "
                "gsva_writebacks=4 lookup_ms=3 map_read_ms=1 overhead_ms=4",
                result.stdout,
            )
            self.assertNotIn("issue:", result.stdout)

            prefix_suffix_fork = write_case(
                "prefix-suffix-fork",
                (
                    "memory_service_summary: service=lingqu_memory_service "
                    "records=10 steps=2/2 "
                    "stages=qwen3_w5_memory_decision_contract:8,"
                    "qwen3_w5_memory_prefix_cache_kv_loaded:1 "
                    "shortpath_ids=runtime_service_catalog support_ids=boundary_registry "
                    "actions=jump-to-terminal artifact_kinds=logits prefetch_ids=none "
                    "prefix_cache_ids=prefix-cache-reuse/runtime-test "
                    "prefix_cache_actions=reuse prefix_cache_kv_hits=1 "
                    "prefix_cache_kv_nodes=1 prefix_cache_gsva_rejections=0 "
                    "gsva_kv_refs=5 gsva_reads=1 gsva_writebacks=4 "
                    "gsva_kv_nodes=1 lookup_hits=2 "
                    "hit_registry_indexes=none hit_registry_steps=none hit_positions=none"
                ),
                35,
                45,
                2,
                14,
                2,
                1,
                0,
                "gsva_timing: records=1 lookup_ms=3 map_read_ms=1 avoided_compute_ms=0",
                token_ids="[81378, 999]",
            )

            strict_result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compare-prefix-cache-benefit",
                    str(baseline),
                    str(prefix_suffix_fork),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(1, strict_result.returncode)
            self.assertIn("baseline/prefix decode_output mismatch", strict_result.stdout)

            relaxed_result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--compare-prefix-cache-benefit",
                    str(baseline),
                    str(prefix_suffix_fork),
                    "--allow-prefix-cache-output-mismatch",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertIn("w5_prefix_cache_benefit: status=pass", relaxed_result.stdout)
            self.assertNotIn("decode_output mismatch", relaxed_result.stdout)

    def test_reports_fused_simt_context_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            (logs_dir / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                context_lines=(
                    "fused_simt_vendor_context_summary: records=2 steps=2/2 "
                    "modes=fused-simt-vendor-object-ref,fused-simt-vendor-paper-object-ref "
                    "max_latency_ms=13 max_latency_step=1 max_latency_node=nodeA "
                    "total_latency_ms=24 output_checksum_xor=0x0000000000000003 "
                    "row_prefetch_hits=4 row_prefetch_requests=4 "
                    "row_prefetch_hit_rate_milli=1000 table_bytes_moved=49152 "
                    "gate_weight_bytes_moved=8192 indices_bytes_moved=32 "
                    "hidden_input_bytes=8192 hidden_output_bytes=8192 "
                    "hidden_injection_overhead_bytes=16384",
                ),
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--require-context",
                    "fused_simt_vendor_context",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            'context_guard: status=pass required_contexts=["fused_simt_vendor_context"]',
            result.stdout,
        )
        self.assertIn(
            "context: label=fused_simt_vendor_context records=2 steps=2/2 "
            "modes=fused-simt-vendor-object-ref,fused-simt-vendor-paper-object-ref "
            "max_latency_ms=13 max_latency_step=1 max_latency_node=nodeA "
            "total_latency_ms=24 output_checksum_xor=0x0000000000000003 "
            "row_prefetch_hits=4 row_prefetch_requests=4 "
            "row_prefetch_hit_rate_milli=1000 table_bytes_moved=49152 "
            "gate_weight_bytes_moved=8192 indices_bytes_moved=32 "
            "hidden_input_bytes=8192 hidden_output_bytes=8192 "
            "hidden_injection_overhead_bytes=16384",
            result.stdout,
        )
        self.assertNotIn("issue:", result.stdout)

    def test_fails_when_required_context_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(summary, logs_dir)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--require-context",
                    "fused_simt_vendor_context",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            'context_guard: status=fail required_contexts=["fused_simt_vendor_context"]',
            result.stdout,
        )
        self.assertIn(
            "issue: context guard: required context missing: fused_simt_vendor_context",
            result.stdout,
        )

    def test_fails_when_required_context_has_partial_step_coverage(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                context_lines=(
                    "paper_engram_context_summary: records=1 steps=1/2 "
                    "modes=simpler-host-paper-object-ref max_latency_ms=7 "
                    "max_latency_step=0 max_latency_node=nodeA total_latency_ms=7 "
                    "output_checksum_xor=0x0000000000000003",
                ),
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--require-context",
                    "paper_engram_context",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "issue: context guard: required context step coverage mismatch: "
            "paper_engram_context value=1/2 expected=2/2",
            result.stdout,
        )

    def test_fails_when_vendor_context_uses_non_vendor_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs" / "run_headless8"
            out_dir.mkdir()
            logs_dir.mkdir(parents=True)
            run_id = "run"
            write_artifacts(out_dir, run_id)
            summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
            write_summary(
                summary,
                logs_dir,
                context_lines=(
                    "fused_simt_vendor_context_summary: records=2 steps=2/2 "
                    "modes=fused-simt-abi-reference-paper-object-ref "
                    "max_latency_ms=13 max_latency_step=1 max_latency_node=nodeA "
                    "total_latency_ms=24 output_checksum_xor=0x0000000000000003 "
                    "row_prefetch_hits=4 row_prefetch_requests=4 "
                    "table_bytes_moved=49152 hidden_injection_overhead_bytes=16384",
                ),
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(summary),
                    "--require-context",
                    "fused_simt_vendor_context",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "issue: context guard: required fused SIMT vendor context has "
            "non-vendor mode: fused-simt-abi-reference-paper-object-ref",
            result.stdout,
        )

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
