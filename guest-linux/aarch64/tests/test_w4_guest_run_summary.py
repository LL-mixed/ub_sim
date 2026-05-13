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
        self.assertIn("decode_output: token_ids=[11, 358]", result.stdout)
        self.assertIn('decode_token: step=1 node=nodeH token=358 piece="\u0120I"', result.stdout)
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
            "progress: elapsed_s=180 expected_decode_steps=2 "
            "node_passes=nodeA=2/2,nodeB=2/2,nodeC=2/2,nodeD=2/2,"
            "nodeE=2/2,nodeF=2/2,nodeG=2/2,nodeH=2/2",
            progress.stdout,
        )
        self.assertIn(
            'progress: terminal_tokens=2/2 latest_token_step=1 token=358 piece="\u0120I"',
            progress.stdout,
        )
        self.assertIn("progress: slowest_node=nodeA slowest_passes=2/2", progress.stdout)


if __name__ == "__main__":
    unittest.main()
