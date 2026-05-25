#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "w5_cluster_health_check.py"
SCRIPT_DIR = SCRIPT.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import w5_cluster_health_check  # noqa: E402


def write_pass_run(out_dir, logs_dir, run_id, reusable=False):
    logs = logs_dir / f"{run_id}_headless8"
    logs.mkdir(parents=True)
    (logs / "nodeA_guest.log").write_text("log\n", encoding="utf-8")
    registry = out_dir / f"w5_memory_registry.{run_id}"
    registry.mkdir()
    (registry / "w5_memory_shortpath_stream.txt").write_text("stream", encoding="utf-8")
    (registry / "w5_memory_shortpath_kv_stream.txt").write_text("kv", encoding="utf-8")
    (out_dir / f"w5_memory_object_store.{run_id}.json").write_text("{}", encoding="utf-8")
    (out_dir / f"w5_object_service_store.{run_id}.json").write_text("{}", encoding="utf-8")
    (out_dir / f"w5_object_service_store.{run_id}.bin").write_bytes(b"binary")
    if reusable:
        (out_dir / f"w5_memory_runtime_boundary_lookup.{run_id}.json").write_text(
            "{}", encoding="utf-8"
        )
    lines = [
        f"summary: run_dir={logs}",
        (
            "summary: decode_steps_expected=2 decode_steps_observed=2 "
            "worker_timing_records=2 passed_nodes=8/8 handoff_timing_records=2 "
            "idle_timing_records=14 engram_timing_records=2 engram_context_records=2"
        ),
        "decode_output: token_ids=[11, 22]",
        'decode_output: token_pieces=", ok"',
        (
            "timing_step: step=0 round_ms=100 critical_node=nodeA workers=1/8 "
            "max_compute_window_ms=70 max_publish_ms=5"
        ),
        (
            "timing_step: step=1 round_ms=40 critical_node=nodeA workers=1/8 "
            "max_compute_window_ms=30 max_publish_ms=4"
        ),
        "engram_timing_step: step=0 engram_total_ms=1",
        "engram_timing_step: step=1 engram_total_ms=1",
        (
            "memory_service_summary: service=lingqu_memory_service records=18 steps=2/2 "
            "actions=jump-to-terminal artifact_kinds=logits lookup_hits=2"
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
    if reusable:
        lines.extend(
            [
                (
                    "memory_boundary_observation_summary: records=14 steps=2/2 "
                    "nodes=node1,node2,node3,node4,node5,node6,node7 "
                    "source=w5_guest_range_exit hidden_backend=obmm_shmem"
                ),
                "memory_boundary_observation: phase=range_exit step=0 node=node1 status=ok",
            ]
        )
    (out_dir / f"eight_node_w5_inference_cluster_summary.{run_id}.txt").write_text(
        "\n".join(lines) + "\n",
        encoding="utf-8",
    )


class W5ClusterHealthCheckTest(unittest.TestCase):
    def test_passes_with_latest_report_and_reusable_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs"
            out_dir.mkdir()
            logs_dir.mkdir()
            reusable = "2026-05-26_00-29-50_w5_qwen3_14b_engram_decode_25060"
            latest = "2026-05-26_03-14-03_w5_qwen3_14b_engram_decode_32556"
            write_pass_run(out_dir, logs_dir, reusable, reusable=True)
            write_pass_run(out_dir, logs_dir, latest)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--out-dir",
                    str(out_dir),
                    "--logs-dir",
                    str(logs_dir),
                    "--skip-qemu-check",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn(f"latest_summary: run_id={latest} status=pass", result.stdout)
        self.assertIn(f"reusable_source: count=1 latest={reusable}", result.stdout)
        self.assertIn("latest_shortpath: lookup_hits=2 actual_range_forwards=2", result.stdout)
        self.assertIn("w5_health_check: status=pass profile=qwen3_14b_engram_decode", result.stdout)

    def test_fails_without_reusable_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs"
            out_dir.mkdir()
            logs_dir.mkdir()
            latest = "2026-05-26_03-14-03_w5_qwen3_14b_engram_decode_32556"
            write_pass_run(out_dir, logs_dir, latest)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--out-dir",
                    str(out_dir),
                    "--logs-dir",
                    str(logs_dir),
                    "--skip-qemu-check",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "issue: no reusable boundary source found for profile=qwen3_14b_engram_decode",
            result.stdout,
        )
        self.assertIn("w5_health_check: status=fail profile=qwen3_14b_engram_decode", result.stdout)

    def test_fails_when_prune_footprint_exceeds_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs"
            out_dir.mkdir()
            logs_dir.mkdir()
            reusable = "2026-05-26_00-29-50_w5_qwen3_14b_engram_decode_25060"
            old_run = "2026-05-26_03-02-33_w5_qwen3_14b_engram_decode_11903"
            latest = "2026-05-26_03-14-03_w5_qwen3_14b_engram_decode_32556"
            write_pass_run(out_dir, logs_dir, reusable, reusable=True)
            write_pass_run(out_dir, logs_dir, old_run)
            write_pass_run(out_dir, logs_dir, latest)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--out-dir",
                    str(out_dir),
                    "--logs-dir",
                    str(logs_dir),
                    "--skip-qemu-check",
                    "--keep-latest",
                    "1",
                    "--max-prune-candidates",
                    "0",
                    "--max-prune-bytes",
                    "0",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("prune_footprint: runs=3 keep_latest=1 prune_candidates=1", result.stdout)
        self.assertIn(
            "issue: prune candidate count exceeds limit: actual=1 limit=0",
            result.stdout,
        )
        self.assertIn("issue: prune footprint exceeds limit:", result.stdout)
        self.assertIn("w5_health_check: status=fail profile=qwen3_14b_engram_decode", result.stdout)

    def test_qemu_check_falls_back_to_ps_when_pgrep_is_unavailable(self):
        responses = [
            subprocess.CompletedProcess(
                ["pgrep", "-fl", "qemu-system-aarch64"],
                3,
                stdout="",
                stderr="sysmon request failed with error: sysmond service not found\n",
            ),
            subprocess.CompletedProcess(
                ["ps", "-axo", "pid=,command="],
                0,
                stdout=(
                    "100 /usr/bin/zsh\n"
                    "200 /Volumes/repos/ub_sim/vendor/qemu/build/qemu-system-aarch64 -machine virt\n"
                ),
                stderr="",
            ),
        ]

        with mock.patch("w5_cluster_health_check.subprocess.run", side_effect=responses):
            lines, unavailable = w5_cluster_health_check.qemu_processes()

        self.assertEqual(unavailable, "")
        self.assertEqual(
            lines,
            ["200 /Volumes/repos/ub_sim/vendor/qemu/build/qemu-system-aarch64 -machine virt"],
        )

    def test_fails_when_w5_pid_file_residue_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            logs_dir = Path(tmp) / "logs"
            out_dir.mkdir()
            logs_dir.mkdir()
            reusable = "2026-05-26_00-29-50_w5_qwen3_14b_engram_decode_25060"
            latest = "2026-05-26_03-14-03_w5_qwen3_14b_engram_decode_32556"
            write_pass_run(out_dir, logs_dir, reusable, reusable=True)
            write_pass_run(out_dir, logs_dir, latest)
            (out_dir / f"ub_nodeA.headless.{latest}.pid").write_text("12345\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--out-dir",
                    str(out_dir),
                    "--logs-dir",
                    str(logs_dir),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertIn("qemu_pid_residue: count=1", result.stdout)
        self.assertIn(f"qemu_pid_residue_line: run_id={latest} pid=12345", result.stdout)
        self.assertIn("issue: qemu pid-file residue detected count=1", result.stdout)

    def test_qemu_check_reports_unavailable_when_pgrep_and_ps_are_blocked(self):
        responses = [
            subprocess.CompletedProcess(
                ["pgrep", "-fl", "qemu-system-aarch64"],
                3,
                stdout="",
                stderr="sysmon request failed with error: sysmond service not found\n",
            ),
            PermissionError(1, "Operation not permitted", "ps"),
        ]

        with mock.patch("w5_cluster_health_check.subprocess.run", side_effect=responses):
            lines, unavailable = w5_cluster_health_check.qemu_processes()

        self.assertEqual(lines, [])
        self.assertIn("sysmond service not found", unavailable)
        self.assertNotIn("\n", unavailable)
        self.assertIn("fallback ps_failed_exception=PermissionError", unavailable)


if __name__ == "__main__":
    unittest.main()
