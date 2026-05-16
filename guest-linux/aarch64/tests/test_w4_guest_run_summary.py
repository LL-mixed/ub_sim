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


def handoff_timing(node_id, node_index, step, found_to_handoff_ms, dispatch_ms, publish_ms):
    return (
        "[w4_guest] stage qwen3_worker_handoff_timing "
        f"local={node_id} step={step} node={node_index} source={max(node_index - 1, 0)} "
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
        "kv_resolve_ms=2 kv_load_ms=1 compute_window_ms=30 submit_ms=7 "
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
        "status=ok"
    )


class W4GuestRunSummaryTest(unittest.TestCase):
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
                (run_dir / f"{node_id}_guest.log").write_text("\n".join(lines) + "\n")
                if node_id == "nodeH":
                    qemu_lines = [
                        "qwen3-engram-context: mode=cpu-reference table_rows=16 "
                        "output_checksum=0x11 gate_checksum=0x21 index_checksum=0x31 "
                        "output_l1_milli=1024 latency_ms=1",
                        "qwen3-engram-context: mode=cpu-reference table_rows=16 "
                        "output_checksum=0x22 gate_checksum=0x42 index_checksum=0x62 "
                        "output_l1_milli=2048 latency_ms=2",
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
        self.assertIn(
            "engram_context_summary: records=2 steps=2/2 modes=cpu-reference "
            "max_latency_ms=2 max_latency_step=1 max_latency_node=nodeH "
            "total_latency_ms=3 output_checksum_xor=0x0000000000000033",
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


if __name__ == "__main__":
    unittest.main()
