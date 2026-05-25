#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "w5_artifact_prune.py"


def write_run(out_dir, logs_dir, run_id, reusable=False):
    summary = out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt"
    lines = [
        "summary: decode_steps_expected=16 decode_steps_observed=16 passed_nodes=8/8",
    ]
    if reusable:
        lines.extend(
            [
                (
                    "memory_boundary_observation_summary: records=112 steps=16/16 "
                    "nodes=node1,node2,node3,node4,node5,node6,node7 "
                    "source=w5_guest_range_exit hidden_backend=obmm_shmem"
                ),
                (
                    "memory_boundary_observation: phase=range_exit "
                    "observation_id=boundary-observation/run/step0/node1 step=0 "
                    "node=node1 status=ok"
                ),
            ]
        )
        (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text(
            "{}", encoding="utf-8"
        )
    summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    (out_dir / f"w5_memory_object_store.{run_id}.json").write_text("memory", encoding="utf-8")
    (out_dir / f"w5_object_service_store.{run_id}.json").write_text("object", encoding="utf-8")
    (out_dir / f"w5_object_service_store.{run_id}.bin").write_bytes(b"binary")
    (out_dir / f"w5_memory_engram_state.{run_id}.json").write_text("state", encoding="utf-8")
    registry = out_dir / f"w5_memory_registry.{run_id}"
    registry.mkdir()
    (registry / "w5_memory_shortpath_stream.txt").write_text("stream", encoding="utf-8")
    (out_dir / f"headless_eight_node_env.{run_id}.sh").write_text("env", encoding="utf-8")
    (out_dir / f"headless_eight_node_cleanup.{run_id}.sh").write_text("cleanup", encoding="utf-8")
    run_logs = logs_dir / f"{run_id}_headless8"
    run_logs.mkdir()
    (run_logs / "nodeA_guest.log").write_text("log", encoding="utf-8")


class W5ArtifactPruneTest(unittest.TestCase):
    def test_dry_run_keeps_reusable_source_and_latest_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs"
            out_dir.mkdir()
            logs_dir.mkdir()
            reusable = "2026-05-26_00-29-50_w5_qwen3_14b_engram_decode_25060"
            newest = "2026-05-26_03-14-03_w5_qwen3_14b_engram_decode_32556"
            older = "2026-05-26_03-06-16_w5_qwen3_14b_engram_decode_32083"
            write_run(out_dir, logs_dir, reusable, reusable=True)
            write_run(out_dir, logs_dir, newest)
            write_run(out_dir, logs_dir, older)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--out-dir",
                    str(out_dir),
                    "--logs-dir",
                    str(logs_dir),
                    "--keep-latest",
                    "1",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(f"action=keep reason=reusable-boundary-source", result.stdout)
        self.assertIn(f"action=keep reason=latest-1-per-profile", result.stdout)
        self.assertIn(f"action=prune reason=older-than-latest-1-per-profile", result.stdout)
        self.assertIn(f"run_id={older}", result.stdout)
        self.assertIn("w5_artifact_prune: mode=dry-run runs=3 prune_candidates=1", result.stdout)

    def test_delete_removes_only_prune_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs"
            out_dir.mkdir()
            logs_dir.mkdir()
            newest = "2026-05-26_03-14-03_w5_qwen3_14b_engram_decode_32556"
            older = "2026-05-26_03-06-16_w5_qwen3_14b_engram_decode_32083"
            write_run(out_dir, logs_dir, newest)
            write_run(out_dir, logs_dir, older)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--out-dir",
                    str(out_dir),
                    "--logs-dir",
                    str(logs_dir),
                    "--keep-latest",
                    "1",
                    "--delete",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertTrue(
                (out_dir / f"eight_node_w5_inference_cluster_summary.{newest}.txt").exists()
            )
            self.assertFalse(
                (out_dir / f"eight_node_w5_inference_cluster_summary.{older}.txt").exists()
            )
            self.assertTrue((logs_dir / f"{newest}_headless8").exists())
            self.assertFalse((logs_dir / f"{older}_headless8").exists())

        self.assertIn("w5_artifact_prune: mode=delete runs=2 prune_candidates=1", result.stdout)


if __name__ == "__main__":
    unittest.main()
