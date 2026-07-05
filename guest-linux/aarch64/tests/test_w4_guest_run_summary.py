#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


NODE_IDS = ("nodeA", "nodeB", "nodeC", "nodeD", "nodeE", "nodeF", "nodeG", "nodeH")


def worker_timing(node_id, node_index, step, total_ms, input_wait_ms, compute_window_ms):
    return (
        "[w4_guest] stage qwen3_worker_timing "
        f"local={node_id} step={step} node={node_index} layers=[0,1) count=1 next=1 "
        f"total_ms={total_ms} terminal_gate_ms=0 setup_ms=1 obmm_stage_ms=0 "
        f"cluster_ms=0 map_ms=0 seed_payload_ms=0 descriptor_ms=0 "
        f"input_wait_ms={input_wait_ms} compute_window_ms={compute_window_ms} "
        "submit_ms=7 base_submit_ms=0 doorbell_submit_ms=7 max_batch_submit_ms=7 "
        "dispatch_ms=0 doorbell_log_ms=0 batch_sleep_ms=0 post_batch_ms=0 "
        "completion_decode_ms=0 compute_unaccounted_ms=0 publish_ms=3 "
        "verify_publish_ms=3 round_done_ms=0 barrier_ms=0 unaccounted_ms=0"
    )


def handoff_timing(
    node_id,
    node_index,
    step,
    found_to_handoff_ms,
    dispatch_ms,
    publish_ms,
    source=None,
    kv_backend="obmm",
    gsva_lookup_ms=0,
    gsva_map_read_ms=0,
    prefix_cache_avoided_compute_ms=0,
):
    if source is None:
        source = max(node_index - 1, 0)
    return (
        "[w4_guest] stage qwen3_worker_handoff_timing "
        f"local={node_id} step={step} node={node_index} source={source} "
        f"next={node_index + 1} layers=[0,1) timebase=supernode_epoch_ms "
        "clock_offset_ms=1000000 input_wait_start_mono_ms=100 "
        "input_found_mono_ms=110 input_loaded_mono_ms=120 "
        "compute_start_mono_ms=130 compute_done_mono_ms=160 "
        "publish_start_mono_ms=170 verify_done_mono_ms=173 "
        "range_publish_start_mono_ms=174 range_publish_done_mono_ms=190 "
        "terminal_publish_start_mono_ms=0 terminal_publish_done_mono_ms=0 "
        "publish_done_mono_ms=191 round_done_start_mono_ms=192 round_done_done_mono_ms=193 "
        "input_found_supernode_ms=1000110 handoff_publish_supernode_ms=1000190 "
        "publish_done_supernode_ms=1000191 producer_publish_supernode_ms=1000100 "
        "producer_publish_mono_ms=100 producer_clock_offset_ms=1000000 "
        "producer_to_input_found_supernode_ms=10 producer_to_input_found_mono_ms=10 "
        "input_wait_ms=10 input_activate_ms=0 "
        "input_metadata_ms=1 input_wait_attempts=1 "
        f"input_found_to_handoff_ms={found_to_handoff_ms} "
        f"input_loaded_to_handoff_ms={max(found_to_handoff_ms - 10, 0)} "
        "kv_resolve_ms=2 kv_load_ms=1 "
        f"kv_backend={kv_backend} gsva_lookup_ms={gsva_lookup_ms} "
        f"gsva_map_read_ms={gsva_map_read_ms} "
        f"prefix_cache_avoided_compute_ms={prefix_cache_avoided_compute_ms} "
        "compute_window_ms=30 submit_ms=7 "
        f"dispatch_ms={dispatch_ms} completion_decode_ms=0 verify_dispatch_ms=3 "
        f"range_publish_ms={publish_ms} terminal_publish_ms=0 compute_done_to_handoff_ms=30 "
        "round_done_publish_ms=1 status=ok"
    )


def engram_timing(node_id, node_index, step):
    return (
        "[w4_guest] stage qwen3_engram_timing "
        f"local={node_id} step={step} node={node_index} owner=node8 "
        f"candidate_publish_ms={2 if node_index == 8 else 0} "
        f"candidate_wait_ms={3 if node_index == 8 else 0} "
        f"policy_select_ms={4 if node_index == 8 else 0} "
        f"decision_publish_ms={5 if node_index == 8 else 0} "
        f"selected_wait_ms={6 if node_index == 8 else 0} "
        f"selected_writeback_ms={7 if node_index == 8 else 0} "
        f"history_state_wait_ms={8 if node_index in (1, 8) else 0} "
        f"qwen3_range_publish_ms={node_index} "
        f"qwen3_range_input_wait_ms={node_index * 10} "
        "status=ok work_item=range_or_shortpath"
    )


def idle_engram_timing(node_id, node_index, step, terminal_wait_ms):
    return (
        "[w4_guest] stage qwen3_engram_timing "
        f"local={node_id} step={step} node={node_index} owner=node8 "
        "candidate_publish_ms=0 candidate_wait_ms=0 policy_select_ms=0 "
        "decision_publish_ms=0 selected_wait_ms=0 selected_writeback_ms=0 "
        "history_state_wait_ms=0 qwen3_range_publish_ms=0 "
        "qwen3_range_input_wait_ms=0 status=idle work_item=none "
        f"terminal_commit_wait_ms={terminal_wait_ms}"
    )


class W4GuestRunSummaryTest(unittest.TestCase):
    def test_emits_worker_shortpath_summary_from_guest_logs(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            for index, node_id in enumerate(NODE_IDS, start=1):
                lines = []
                for step in range(2):
                    if node_id == "nodeA":
                        lines.extend(
                            [
                                (
                                    "[w4_guest] stage uapi_qwen3_range_runtime_forward "
                                    f"local={node_id} step={step} node={index} status=ok"
                                ),
                                worker_timing(node_id, index, step, 1000 + step, step, 900),
                                handoff_timing(node_id, index, step, 950 + step, 0, 7),
                                (
                                    "[w4_guest] stage qwen3_range_forward_runtime_input_loaded "
                                    f"local={node_id} step={step} status=ok"
                                )
                                if step > 0
                                else "",
                                (
                                    "[w4_guest] stage qwen3_w5_memory_terminal_logits_loaded "
                                    f"local={node_id} step={step} decision_id=shortpath-decision/step{step} "
                                    "artifact_id=artifact/logits status=ok"
                                ),
                                (
                                    "[w4_guest] stage qwen3_w5_memory_terminal_logits_selected "
                                    f"local={node_id} step={step} token=11 status=ok"
                                ),
                                (
                                    "[w4_guest] stage qwen3_w5_memory_shortpath_commit "
                                    f"local={node_id} step={step} publish_hidden=0 status=ok"
                                ),
                                "[w4_guest] pass",
                            ]
                        )
                    else:
                        lines.extend(
                            [
                                (
                                    "[w4_guest] stage qwen3_decode_round_scheduler_no_dispatch "
                                    f"local={node_id} step={step} status=ok"
                                ),
                                (
                                    "[w4_guest] stage qwen3_decode_round_terminal_committed "
                                    f"local={node_id} step={step} terminal_observed=1 status=ok"
                                ),
                                (
                                    "[w4_guest] stage qwen3_decode_round_idle_timing "
                                    f"local={node_id} step={step} node={index} terminal_observed=1 "
                                    "input_wait_ms=50 round_done_ms=1 source=shortpath status=no_work_item"
                                ),
                                "[w4_guest] pass",
                            ]
                        )
                (run_dir / f"{node_id}_guest.log").write_text(
                    "\n".join(line for line in lines if line) + "\n",
                    encoding="utf-8",
                )

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "2", *NODE_IDS],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "guest_worker_shortpath_summary: action=jump-to-terminal "
            "boundary_hits=2 terminal_selects=2 expected_hits=2 "
            "actual_range_forwards=2 actual_runtime_inputs=1 actual_runtime_outputs=0 "
            "shortpath_no_dispatch=14 shortpath_terminal_commits=14 "
            "shortpath_publish_hidden_zero=2 full_pipeline_range_forwards=16 "
            "full_pipeline_runtime_inputs=15 full_pipeline_runtime_outputs=16",
            result.stdout,
        )

    def test_emits_decode_tokens_and_timing_bottlenecks(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            for index, node_id in enumerate(NODE_IDS, start=1):
                total0 = 100 * index
                total1 = 200 * index
                barrier0 = 10 * index
                lines = [
                    worker_timing(node_id, index, 0, total0, 50 * index, 5 * index),
                    (
                        "[w4_guest] stage qwen3_worker_barrier_timing "
                        f"local={node_id} step=0 node={index} "
                        f"barrier_ms={barrier0} total_with_barrier_ms={total0 + barrier0}"
                    ),
                    (
                        "[w4_guest] stage qwen3_obmm_pool_usage "
                        f"local=node{index} step=0 per_node_region_bytes=536870912 "
                        "cluster_region_bytes=4294967296 payload_bytes=536834048 "
                        f"payload_high_water_bytes={1048576 + index * 4096} "
                        "payload_used_pct_milli=200 arena_base=0x0000000000100000 "
                        f"arena_used_bytes={index * 4096} "
                        f"arena_next=0x{1048576 + index * 4096:016x} "
                        "allocator=linear_payload_arena status=ok"
                    ),
                    "[w4_guest] pass",
                    worker_timing(node_id, index, 1, total1, 70 * index, 9 * index),
                    handoff_timing(node_id, index, 1, 80 * index, 11 * index, 5 * index),
                    (
                        "[w4_guest] stage qwen3_range_forward_runtime_ingress_publish "
                        f"local=node{index} target=node{index + 1} step=1 "
                        f"observation_id=boundary-observation/run-from-guest/step1/node{index} "
                        f"key=hidden/qwen3-0-6b/node{index + 1}/range-runtime-input/decode-step1 "
                        f"key_hash=0x{index:016x} version=1 layers=[{index},{index + 1}) "
                        f"count=1 checksum=0x{index + 16:016x} bytes=262144 "
                        "producer_publish_ms=1000 producer_publish_mono_ms=2000 "
                        "producer_clock_offset_ms=3000 epoch=2 seq=1 backing=obmm_shmem "
                        "metadata=lingqu_object_service queue=obmm_spsc status=ok"
                    ),
                    (
                        "[w4_guest] stage qwen3_w5_memory_boundary_decision "
                        f"local={node_id} step=1 layers=[0,1) next=node{index + 1} "
                        "shortpath_id=shortpath-decision/boundary/step1 "
                        "shortpath_support_id=shortpath-support/boundary/step1 "
                        "shortpath_action=jump-to-terminal "
                        "shortpath_artifact_kind=logits "
                        "shortpath_artifact_checksum=0xabc "
                        "shortpath_artifact_ref_chars=128 "
                        "prefetch_id=prefetch-plan/step1 prefetch_scope=multi-step "
                        "prefetch_target_step=2 prefetch_artifact_ids=artifact/kv/step2 "
                        "prefetch_artifact_checksums=0xdef prefetch_artifact_refs_chars=128 "
                        "prefix_cache_id=prefix-cache-reuse/step1 "
                        "prefix_cache_action=reuse "
                        "prefix_cache_artifact_checksum=0x123 "
                        "prefix_cache_artifact_ref_chars=128 "
                        "source=lingqu_memory_service "
                        "target=range_forward_boundary status=validated"
                    ),
                    (
                        "[w4_guest] stage qwen3_w5_memory_terminal_logits_publish_early "
                        f"node={index} step=1 layers=[0,1) "
                        "decision_id=shortpath-decision/boundary/step1 "
                        "artifact_id=artifact/logits/step1 token=358 "
                        "source=lingqu_memory_service "
                        "target=terminal_token_result status=ok"
                    )
                    if node_id == "nodeC"
                    else "",
                    engram_timing(node_id, index, 1),
                    (
                        "[w4_guest] stage qwen3_obmm_pool_usage "
                        f"local=node{index} step=1 per_node_region_bytes=536870912 "
                        "cluster_region_bytes=4294967296 payload_bytes=536834048 "
                        f"payload_high_water_bytes={2097152 + index * 8192} "
                        "payload_used_pct_milli=400 arena_base=0x0000000000100000 "
                        f"arena_used_bytes={1048576 + index * 8192} "
                        f"arena_next=0x{2097152 + index * 8192:016x} "
                        "allocator=linear_payload_arena status=ok"
                    ),
                    "[w4_guest] pass",
                ]
                lines.insert(1, handoff_timing(node_id, index, 0, 40 * index, 10 * index, 3 * index))
                if node_id == "nodeH":
                    lines.insert(
                        0,
                        "[w4_guest] stage qwen3_terminal_token_result_publish "
                        "local=node8 step=0 token=11 runner_up=0 margin_milli=122 "
                        "logits_checksum=0x1 text_checksum=0x2 "
                        "piece_word0=0x000000000000002c piece_word1=0x0000000000000000 "
                        "object_key=tokens/qwen3-0.6b/decode-step0 status=ok",
                    )
                    lines.insert(
                        3,
                        "[w4_guest] stage qwen3_terminal_token_result_publish "
                        "local=node8 step=1 token=358 runner_up=1128 margin_milli=1350 "
                        "logits_checksum=0x3 text_checksum=0x4 "
                        "piece_word0=0x000000000049a0c4 piece_word1=0x0000000000000000 "
                        "object_key=tokens/qwen3-0.6b/decode-step1 status=ok",
                    )
                    lines.insert(
                        4,
                        "[w4_guest] stage qwen3_w5_memory_terminal_logits_execute "
                        "node=8 step=1 decision_id=shortpath-decision/boundary/step1 "
                        "artifact_id=artifact/logits/step1 token=358 runner_up=1128 "
                        "margin_milli=1350 logits_checksum=0x3 text_checksum=0x4 "
                        "payload_bytes=1024 payload_checksum=0x99 "
                        "source=lingqu_memory_service "
                        "target=terminal_token_result status=ok",
                    )
                (run_dir / f"{node_id}_guest.log").write_text(
                    "\n".join(line for line in lines if line) + "\n"
                )
                if node_id == "nodeH":
                    qemu_lines = [
                        "qwen3-engram-context: step=0 mode=cpu-reference table_rows=16 "
                        "output_checksum=0x11 gate_checksum=0x21 index_checksum=0x31 "
                        "output_l1_milli=1024 latency_ms=1 "
                        "row_prefetch_hits=1 row_prefetch_requests=2 "
                        "row_prefetch_hit_rate_milli=500 table_bytes_moved=100 "
                        "gate_weight_bytes_moved=10 indices_bytes_moved=0 "
                        "hidden_input_bytes=4 hidden_output_bytes=4 "
                        "hidden_injection_overhead_bytes=8",
                        "qwen3-engram-context: step=1 mode=cpu-reference table_rows=16 "
                        "output_checksum=0x22 gate_checksum=0x42 index_checksum=0x62 "
                        "output_l1_milli=2048 latency_ms=2 "
                        "row_prefetch_hits=2 row_prefetch_requests=2 "
                        "row_prefetch_hit_rate_milli=1000 table_bytes_moved=200 "
                        "gate_weight_bytes_moved=20 indices_bytes_moved=4 "
                        "hidden_input_bytes=4 hidden_output_bytes=4 "
                        "hidden_injection_overhead_bytes=8",
                    ]
                    (run_dir / f"{node_id}_qemu.log").write_text("\n".join(qemu_lines) + "\n")

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "2", *NODE_IDS],
                check=True,
                capture_output=True,
                text=True,
            )
            progress = subprocess.run(
                [sys.executable, str(script), "--progress", str(run_dir), "2", "180", *NODE_IDS],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "summary: decode_steps_expected=2 decode_steps_observed=2 "
            "worker_timing_records=16 passed_nodes=8/8",
            result.stdout,
        )
        self.assertIn("handoff_timing_records=16", result.stdout)
        self.assertIn("engram_timing_records=8", result.stdout)
        self.assertIn("engram_context_records=2", result.stdout)
        self.assertIn("decode_output: token_ids=[11, 358]", result.stdout)
        self.assertIn('decode_output: token_pieces=", I"', result.stdout)
        self.assertIn('decode_token: step=1 node=nodeH token=358 piece=" I"', result.stdout)
        self.assertIn(
            "timing_step: step=0 round_ms=880 critical_node=nodeH workers=8/8",
            result.stdout,
        )
        self.assertIn(
            "timing_bottleneck: max_input_wait_step=1 node=nodeH "
            "input_wait_ms=560 worker_total_ms=1600",
            result.stdout,
        )
        self.assertIn(
            "handoff_step: step=1 workers=8/8 critical_node=nodeH "
            "input_found_to_handoff_ms=640",
            result.stdout,
        )
        self.assertIn(
            "handoff_bottleneck: max_handoff_step=1 node=nodeH "
            "input_found_to_handoff_ms=640",
            result.stdout,
        )
        self.assertIn(
            "edge_step: step=1 edges=7/7 total_edge_gap_mono_ms=70 "
            "total_edge_gap_mono_raw_ms=70 max_edge_gap_mono_ms=10 max_edge=1->2",
            result.stdout,
        )
        self.assertIn(
            "edge_bottleneck: max_edge_step=0 edge=1->2 node=nodeB "
            "producer_to_input_found_mono_ms=10",
            result.stdout,
        )
        self.assertIn(
            "engram_timing_step: step=1 nodes=8/8 candidate_publish_ms=2 "
            "candidate_wait_ms=3 policy_select_ms=4 decision_publish_ms=5 "
            "selected_wait_ms=6 selected_writeback_ms=7 history_state_wait_ms=16 "
            "engram_total_ms=43 max_qwen3_range_publish_ms=8 "
            "max_qwen3_range_input_wait_ms=80 bottleneck=range_pipeline bottleneck_ms=88",
            result.stdout,
        )
        self.assertIn("bottleneck_ms=88 idle_nodes=0", result.stdout)
        self.assertIn(
            "engram_bottleneck: dominant=range_pipeline dominant_ms=88 "
            "cpu_policy_ms=4 object_transport_ms=39 range_pipeline_ms=88",
            result.stdout,
        )
        self.assertIn(
            "engram_timing_node: node=nodeH steps=1/2 candidate_publish_ms=2 "
            "candidate_wait_ms=3 policy_select_ms=4 decision_publish_ms=5",
            result.stdout,
        )
        self.assertIn("max_qwen3_range_input_wait_ms=80 idle_steps=0", result.stdout)
        self.assertIn(
            "engram_context_summary: records=2 steps=2/2 modes=cpu-reference "
            "max_latency_ms=2 max_latency_step=1 max_latency_node=nodeH "
            "total_latency_ms=3 output_checksum_xor=0x0000000000000033",
            result.stdout,
        )
        self.assertIn(
            "row_prefetch_hits=3 row_prefetch_requests=4 row_prefetch_hit_rate_milli=750 "
            "table_bytes_moved=300 gate_weight_bytes_moved=30 indices_bytes_moved=4 "
            "hidden_input_bytes=8 hidden_output_bytes=8 hidden_injection_overhead_bytes=16",
            result.stdout,
        )
        self.assertIn(
            "engram_context_step: step=1 node=nodeH mode=cpu-reference table_rows=16 "
            "output_checksum=0x22 gate_checksum=0x42 index_checksum=0x62 "
            "output_l1_milli=2048 latency_ms=2 row_prefetch_hits=2 "
            "row_prefetch_requests=2 row_prefetch_hit_rate_milli=1000 "
            "table_bytes_moved=200 gate_weight_bytes_moved=20 indices_bytes_moved=4 "
            "hidden_input_bytes=4 hidden_output_bytes=4 hidden_injection_overhead_bytes=8",
            result.stdout,
        )
        self.assertIn(
            "memory_service_summary: service=lingqu_memory_service records=10 steps=1/2 "
            "stages=qwen3_w5_memory_boundary_decision:8,"
            "qwen3_w5_memory_terminal_logits_execute:1,"
            "qwen3_w5_memory_terminal_logits_publish_early:1 "
            "shortpath_ids=shortpath-decision/boundary/step1 "
            "support_ids=shortpath-support/boundary/step1 "
            "actions=jump-to-terminal artifact_kinds=logits "
            "prefetch_ids=prefetch-plan/step1 "
            "prefix_cache_ids=prefix-cache-reuse/step1",
            result.stdout,
        )
        self.assertIn(
            "memory_service_step: step=1 boundary_records=8 "
            "nodes=nodeA,nodeB,nodeC,nodeD,nodeE,nodeF,nodeG,nodeH "
            "shortpath_ids=shortpath-decision/boundary/step1 "
            "support_ids=shortpath-support/boundary/step1 "
            "actions=jump-to-terminal prefetch_ids=prefetch-plan/step1 "
            "prefix_cache_ids=prefix-cache-reuse/step1",
            result.stdout,
        )
        self.assertIn(
            "memory_boundary_observation_summary: records=8 steps=1/2 "
            "nodes=node1,node2,node3,node4,node5,node6,node7,node8 "
            "targets=node2,node3,node4,node5,node6,node7,node8,node9 "
            "source=w5_guest_range_exit hidden_backend=obmm_shmem",
            result.stdout,
        )
        self.assertIn(
            "memory_boundary_observation: phase=range_exit "
            "observation_id=boundary-observation/run-from-guest/step1/node1 "
            "step=1 node=node1 "
            "target=node2 layers=[1,2) layer_start=1 layer_end=2 layer_count=1 "
            "hidden_key=hidden/qwen3-0-6b/node2/range-runtime-input/decode-step1",
            result.stdout,
        )
        self.assertIn(
            "engram_context_step: step=1 node=nodeH mode=cpu-reference "
            "table_rows=16 output_checksum=0x22 gate_checksum=0x42 "
            "index_checksum=0x62 output_l1_milli=2048 latency_ms=2",
            result.stdout,
        )
        self.assertIn(
            "obmm_pool: nodes_observed=8/8 expected_steps=2 "
            "per_node_region_bytes=536870912 cluster_region_bytes=4294967296",
            result.stdout,
        )
        self.assertIn(
            "obmm_pool_node: node=nodeH step=1 payload_bytes=536834048 "
            "payload_high_water_bytes=2162688",
            result.stdout,
        )
        self.assertIn(
            'progress: elapsed=03:00 cluster_decode=2/2 (100%) terminal_tokens=2/2 '
            'latest_token=step=1 token=358 piece=" I"',
            progress.stdout,
        )
        self.assertIn(
            "progress: cluster_bar=[########################] node_range=2..2/2 "
            "lagging=nodeA",
            progress.stdout,
        )
        self.assertIn(
            "progress: node_passes A=2/2 B=2/2 C=2/2 D=2/2 E=2/2 F=2/2 G=2/2 H=2/2",
            progress.stdout,
        )
        self.assertIn(
            "progress: lagging_status node=nodeA passes=2/2",
            progress.stdout,
        )

    def test_handoff_edges_exclude_terminal_token_inputs(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "nodeA_guest.log").write_text(
                "\n".join(
                    [
                        worker_timing("nodeA", 1, 0, 100, 0, 10),
                        handoff_timing("nodeA", 1, 0, 80, 1, 3, source=1),
                        "[w4_guest] pass",
                    ]
                )
                + "\n"
            )
            (run_dir / "nodeB_guest.log").write_text(
                "\n".join(
                    [
                        worker_timing("nodeB", 2, 0, 120, 10, 12),
                        handoff_timing("nodeB", 2, 0, 90, 1, 3, source=1),
                        "[w4_guest] pass",
                    ]
                )
                + "\n"
            )

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "1", "nodeA", "nodeB"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("edge_step: step=0 edges=1/1", result.stdout)
        self.assertNotIn("edges=2/1", result.stdout)
        self.assertIn("edge_bottleneck: max_edge_step=0 edge=1->2 node=nodeB", result.stdout)

    def test_memory_service_summary_reports_actual_lookup_hits(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "nodeA_guest.log").write_text(
                "\n".join(
                    [
                        (
                            "[w4_guest] stage qwen3_w5_memory_boundary_decision "
                            "local=nodeA step=1 layers=[0,5) next=node2 "
                            "shortpath_id=runtime_service_catalog "
                            "lookup_backend=staged_registry lookup_mode=staged_registry "
                            "shortpath_support_id=boundary_registry "
                            "shortpath_action=jump-to-terminal "
                            "shortpath_artifact_kind=logits "
                            "shortpath_artifact_checksum=none "
                            "shortpath_artifact_ref_chars=0 "
                            "shortpath_catalog_entries=112 "
                            "prefetch_id=none prefetch_scope=none "
                            "prefetch_target_step=none prefetch_artifact_ids=none "
                            "prefetch_artifact_checksums=none "
                            "prefetch_artifact_refs_chars=0 "
                            "prefix_cache_id=none prefix_cache_action=none "
                            "prefix_cache_artifact_checksum=none "
                            "prefix_cache_artifact_ref_chars=0 "
                            "source=lingqu_memory_service "
                            "target=range_forward_boundary status=validated"
                        ),
                        (
                            "[w4_guest] stage qwen3_memory_service_boundary_lookup_response "
                            "node=1 step=1 layers=[0,5) position=4 "
                            "action=jump-to-terminal artifact_ref=boundary_registry "
                            "registry_index=7 registry_step=1 registry_position=4 "
                            "registry_layers=[0,5) confidence=verified "
                            "source=lingqu_memory_service "
                            "target=boundary_controller mode=staged_registry "
                            "backend=staged_registry status=hit"
                        ),
                        "[w4_guest] pass",
                    ]
                )
                + "\n"
            )

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "1", "nodeA"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "memory_service_summary: service=lingqu_memory_service records=2 "
            "steps=1/1 "
            "stages=qwen3_memory_service_boundary_lookup_response:1,"
            "qwen3_w5_memory_boundary_decision:1 "
            "shortpath_ids=runtime_service_catalog "
            "support_ids=boundary_registry actions=jump-to-terminal "
            "artifact_kinds=logits prefetch_ids=none prefix_cache_ids=none "
            "prefix_cache_actions=none prefix_cache_kv_hits=0 "
            "prefix_cache_kv_nodes=none "
            "prefix_cache_gsva_rejections=0 "
            "prefix_cache_gsva_rejection_reasons=none "
            "gsva_kv_refs=0 gsva_reads=0 gsva_writebacks=0 "
            "gsva_kv_nodes=none "
            "lookup_hits=1 hit_registry_indexes=7 hit_registry_steps=1 "
            "hit_positions=4",
            result.stdout,
        )
        self.assertIn(
            "memory_service_step: step=1 boundary_records=1 nodes=nodeA "
            "shortpath_ids=runtime_service_catalog support_ids=boundary_registry "
            "actions=jump-to-terminal prefetch_ids=none prefix_cache_ids=none "
            "prefix_cache_actions=none "
            "lookup_hits=1 hit_registry_indexes=7 hit_registry_steps=1 "
            "hit_positions=4",
            result.stdout,
        )

    def test_memory_service_summary_reports_gsva_kv_activity(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "nodeA_guest.log").write_text(
                "\n".join(
                    [
                        (
                            "[w4_guest] stage qwen3_w5_memory_gsva_kv_loaded "
                            "node=1 step=1 previous_step=0 backend=gsva "
                            "segment_id=gsva/run/node1 base=0x80000000 bytes=2048 "
                            "token=0x1234 epoch=1 retired=0 checksum=0xdef "
                            "source=prefix_cache target=uapi_object_ref status=ok"
                        ),
                        "[w4_guest] pass",
                    ]
                )
                + "\n"
            )
            (run_dir / "nodeA_qemu.log").write_text(
                (
                    "[w4_guest] stage qwen3_w5_memory_gsva_kv_writeback "
                    "node=1 step=0 position=4 layers=[0,4) backend=gsva "
                    "segment_id=gsva/run/node1 base=0x80000000 bytes=2048 "
                    "token=0x1234 epoch=1 retired=0 checksum=0xdef status=ok\n"
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "1", "nodeA"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("gsva_kv_refs=2", result.stdout)
        self.assertIn("gsva_reads=1", result.stdout)
        self.assertIn("gsva_writebacks=1", result.stdout)
        self.assertIn("gsva_kv_nodes=1", result.stdout)

    def test_memory_service_summary_reports_gsva_rejection_and_timing(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "nodeA_guest.log").write_text(
                "\n".join(
                    [
                        (
                            "[w4_guest] stage qwen3_w5_memory_prefix_cache_gsva_rejected "
                            "entry_index=0 accepted_index=0 node=1 step=1 "
                            "request_id=req-stale previous_step=0 "
                            "backend=gsva segment_id=gsva/run/node1 bytes=2048 gsva_bytes=2048 "
                            "token=0x1234 epoch=1 expected_epoch=2 retired=0 "
                            "checksum=0xdef expected_checksum=0xdef reason=epoch_mismatch "
                            "source=prefix_cache target=runtime_recompute status=rejected"
                        ),
                        (
                            "[w4_guest] stage uapi_qwen3_range_runtime_forward "
                            "local=nodeA step=1 node=1 status=ok"
                        ),
                        handoff_timing(
                            "nodeA",
                            1,
                            1,
                            40,
                            5,
                            3,
                            kv_backend="gsva",
                            gsva_lookup_ms=2,
                            gsva_map_read_ms=1,
                        ),
                        "[w4_guest] pass",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "1", "nodeA"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("prefix_cache_gsva_rejections=1", result.stdout)
        self.assertIn("prefix_cache_gsva_rejection_reasons=epoch_mismatch", result.stdout)
        self.assertIn("prefix_cache_reject_policy=cache_reject_then_recompute", result.stdout)
        self.assertIn("prefix_cache_recompute_range_forwards=1", result.stdout)
        self.assertIn("prefix_cache_reject_then_recompute=1", result.stdout)
        self.assertIn(
            "memory_service_request: request_id=req-stale records=1 steps=1",
            result.stdout,
        )
        self.assertIn("prefix_cache_gsva_rejections=1", result.stdout)
        self.assertIn(
            "gsva_timing: records=1 lookup_ms=2 map_read_ms=1 avoided_compute_ms=0",
            result.stdout,
        )

    def test_w5_device_summary_reports_npu_gsva_tensor_consumer(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "nodeA_guest.log").write_text(
                "\n".join(
                    [
                        (
                            "[w4_guest] stage qwen3_w5_device_gsva_tensor_consumer "
                            "device=npu backend=gsva op=vector_add_u32 node=0 peer=1 "
                            "dtype=u32 input_shape=16 output_shape=16 "
                            "input_bytes=128 output_bytes=64 "
                            "cpu_checksum=0x310 device_checksum=0x310 status=ok"
                        ),
                        (
                            "[w4_guest] stage qwen3_w5_device_gsva_tensor_rejected "
                            "device=npu backend=gsva guard=token reason=token_denied "
                            "node=0 peer=1 status=rejected"
                        ),
                        (
                            "[w4_guest] stage qwen3_w5_device_gsva_tensor_rejected "
                            "device=npu backend=gsva guard=epoch reason=stale_epoch "
                            "node=0 peer=1 status=rejected"
                        ),
                        (
                            "[w4_guest] stage qwen3_w5_device_gsva_tensor_rejected "
                            "device=npu backend=gsva guard=retire reason=segment_retired "
                            "node=0 peer=1 status=rejected"
                        ),
                        "[w4_guest] pass",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "1", "nodeA"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "w5_device_summary: records=4 tensor_consumers=1 devices=npu "
            "backends=gsva ops=vector_add_u32 nodes=0 output_shapes=16 "
            "checksum_matches=1 shape_verified=1 rejections=3 "
            "rejection_guards=token,epoch,retire "
            "rejection_reasons=token_denied,stale_epoch,segment_retired status=ok",
            result.stdout,
        )

    def test_idle_engram_timing_does_not_count_terminal_wait_as_range_pipeline(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            for index, node_id in enumerate(NODE_IDS, start=1):
                if index == 1:
                    lines = [
                        "[w4_guest] stage qwen3_engram_timing "
                        "local=nodeA step=0 node=1 owner=node8 "
                        "candidate_publish_ms=0 candidate_wait_ms=0 policy_select_ms=1 "
                        "decision_publish_ms=0 selected_wait_ms=0 selected_writeback_ms=1 "
                        "history_state_wait_ms=0 qwen3_range_publish_ms=7 "
                        "qwen3_range_input_wait_ms=0 status=ok work_item=range_or_shortpath",
                        "[w4_guest] pass",
                    ]
                else:
                    lines = [
                        (
                            "[w4_guest] stage qwen3_decode_round_idle_timing "
                            f"local={node_id} step=0 node={index} terminal_observed=1 "
                            "input_wait_ms=5000 round_done_ms=0 "
                            "source=decode_round_scheduler status=idle"
                        ),
                        idle_engram_timing(node_id, index, 0, 5000),
                        "[w4_guest] pass",
                    ]
                (run_dir / f"{node_id}_guest.log").write_text("\n".join(lines) + "\n")

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "1", *NODE_IDS],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "engram_timing_step: step=0 nodes=8/8 candidate_publish_ms=0 "
            "candidate_wait_ms=0 policy_select_ms=1 decision_publish_ms=0 "
            "selected_wait_ms=0 selected_writeback_ms=1 history_state_wait_ms=0 "
            "engram_total_ms=2 max_qwen3_range_publish_ms=7 "
            "max_qwen3_range_input_wait_ms=0 bottleneck=range_pipeline bottleneck_ms=7 "
            "idle_nodes=7",
            result.stdout,
        )
        self.assertIn(
            "engram_bottleneck: dominant=range_pipeline dominant_ms=7 "
            "cpu_policy_ms=1 object_transport_ms=1 range_pipeline_ms=7",
            result.stdout,
        )
        self.assertIn(
            "engram_timing_node: node=nodeB steps=1/1 candidate_publish_ms=0 "
            "candidate_wait_ms=0 policy_select_ms=0 decision_publish_ms=0 "
            "selected_wait_ms=0 selected_writeback_ms=0 history_state_wait_ms=0 "
            "max_qwen3_range_input_wait_ms=0 idle_steps=1",
            result.stdout,
        )
        self.assertIn("idle_timing_records=7", result.stdout)
        self.assertIn(
            "timing_idle_step: step=0 idle_nodes=7/8 terminal_observed=7/7 "
            "max_terminal_wait_ms=5000 critical_node=nodeB status=no_work_item",
            result.stdout,
        )
        self.assertIn(
            "timing_node: node=nodeB steps=0/1 idle_steps=1/1 "
            "max_terminal_wait_ms=5000 status=idle_no_work_item",
            result.stdout,
        )
        self.assertIn(
            "handoff_node: node=nodeB steps=0/1 idle_steps=1/1 "
            "max_terminal_wait_ms=5000 status=idle_no_work_item",
            result.stdout,
        )
        self.assertIn(
            "obmm_pool: not_observed reason=no_qwen3_obmm_pool_usage_records "
            "active_worker_records=0 idle_worker_records=7",
            result.stdout,
        )
        self.assertNotIn("node=nodeB steps=0/1 status=missing", result.stdout)

    def test_emits_paper_engram_context_prefixed_summary(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            qemu_lines = [
                "qwen3-engram-context: step=0 mode=cpu-reference-object-ref "
                "table_rows=16 output_checksum=0x11 gate_checksum=0x21 "
                "index_checksum=0x31 output_l1_milli=1024 latency_ms=1",
                "qwen3-engram-context: step=1 mode=cpu-reference-paper-object-ref "
                "table_rows=32 output_checksum=0x22 gate_checksum=0x42 "
                "index_checksum=0x62 output_l1_milli=2048 latency_ms=2 "
                "row_prefetch_hits=2 row_prefetch_requests=4 "
                "row_prefetch_hit_rate_milli=500 table_bytes_moved=4096 "
                "gate_weight_bytes_moved=128 indices_bytes_moved=0 "
                "hidden_input_bytes=256 hidden_output_bytes=256 "
                "hidden_injection_overhead_bytes=512",
            ]
            (run_dir / "nodeH_guest.log").write_text("[w4_guest] pass\n")
            (run_dir / "nodeH_qemu.log").write_text("\n".join(qemu_lines) + "\n")

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "2", "nodeH"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "engram_context_records=2 paper_engram_context_records=1",
            result.stdout,
        )
        self.assertIn(
            "engram_context_summary: records=2 steps=2/2 "
            "modes=cpu-reference-object-ref,cpu-reference-paper-object-ref",
            result.stdout,
        )
        self.assertIn(
            "paper_engram_context_summary: records=1 steps=1/2 "
            "modes=cpu-reference-paper-object-ref max_latency_ms=2 "
            "max_latency_step=1 max_latency_node=nodeH total_latency_ms=2 "
            "output_checksum_xor=0x0000000000000022 row_prefetch_hits=2 "
            "row_prefetch_requests=4 row_prefetch_hit_rate_milli=500 "
            "table_bytes_moved=4096 gate_weight_bytes_moved=128 "
            "indices_bytes_moved=0 hidden_input_bytes=256 hidden_output_bytes=256 "
            "hidden_injection_overhead_bytes=512",
            result.stdout,
        )
        self.assertIn(
            "paper_engram_context_step: step=1 node=nodeH "
            "mode=cpu-reference-paper-object-ref table_rows=32 "
            "output_checksum=0x22 gate_checksum=0x42 index_checksum=0x62 "
            "output_l1_milli=2048 latency_ms=2 row_prefetch_hits=2 "
            "row_prefetch_requests=4 row_prefetch_hit_rate_milli=500 "
            "table_bytes_moved=4096 gate_weight_bytes_moved=128 "
            "indices_bytes_moved=0 hidden_input_bytes=256 hidden_output_bytes=256 "
            "hidden_injection_overhead_bytes=512",
            result.stdout,
        )

    def test_emits_fused_simt_vendor_context_prefixed_summary(self):
        script = Path(__file__).resolve().parents[1] / "scripts" / "w4_guest_run_summary.py"

        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            qemu_lines = [
                "qwen3-engram-context: step=0 mode=fused-simt-vendor-object-ref "
                "table_rows=8 output_checksum=0x101 gate_checksum=0x201 "
                "index_checksum=0x301 output_l1_milli=512 latency_ms=11 "
                "table_bytes_moved=32768 gate_weight_bytes_moved=4096 "
                "indices_bytes_moved=32 hidden_input_bytes=4096 "
                "hidden_output_bytes=4096 hidden_injection_overhead_bytes=8192",
                "qwen3-engram-context: step=1 mode=fused-simt-vendor-paper-object-ref "
                "table_rows=16 output_checksum=0x102 gate_checksum=0x202 "
                "index_checksum=0x302 output_l1_milli=768 latency_ms=13 "
                "row_prefetch_hits=4 row_prefetch_requests=4 "
                "row_prefetch_hit_rate_milli=1000 table_bytes_moved=16384 "
                "gate_weight_bytes_moved=4096 indices_bytes_moved=0 "
                "hidden_input_bytes=4096 hidden_output_bytes=4096 "
                "hidden_injection_overhead_bytes=8192",
            ]
            (run_dir / "nodeA_guest.log").write_text("[w4_guest] pass\n")
            (run_dir / "nodeA_qemu.log").write_text("\n".join(qemu_lines) + "\n")

            result = subprocess.run(
                [sys.executable, str(script), str(run_dir), "2", "nodeA"],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(
            "engram_context_records=2 paper_engram_context_records=1 "
            "fused_simt_context_records=2 fused_simt_vendor_context_records=2",
            result.stdout,
        )
        self.assertIn(
            "fused_simt_context_summary: records=2 steps=2/2 "
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
        self.assertIn(
            "fused_simt_vendor_context_summary: records=2 steps=2/2 "
            "modes=fused-simt-vendor-object-ref,fused-simt-vendor-paper-object-ref "
            "max_latency_ms=13 max_latency_step=1 max_latency_node=nodeA "
            "total_latency_ms=24 output_checksum_xor=0x0000000000000003",
            result.stdout,
        )
        self.assertIn(
            "fused_simt_vendor_context_step: step=1 node=nodeA "
            "mode=fused-simt-vendor-paper-object-ref table_rows=16 "
            "output_checksum=0x102 gate_checksum=0x202 index_checksum=0x302 "
            "output_l1_milli=768 latency_ms=13 row_prefetch_hits=4 "
            "row_prefetch_requests=4 row_prefetch_hit_rate_milli=1000 "
            "table_bytes_moved=16384 gate_weight_bytes_moved=4096 "
            "indices_bytes_moved=0 hidden_input_bytes=4096 "
            "hidden_output_bytes=4096 hidden_injection_overhead_bytes=8192",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
