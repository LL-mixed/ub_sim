import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "run_transport_perf_matrix.sh"


def test_transport_perf_matrix_runner_has_stable_dry_run_cli():
    result = subprocess.run(
        [
            str(SCRIPT),
            "--dry-run",
            "--quick",
            "--run-id",
            "unit_transport_perf",
            "--out-dir",
            "/tmp/unit_transport_perf",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert "transport_perf_matrix: run_id=unit_transport_perf" in result.stdout
    assert "transport_perf_matrix: size=262144 iterations=1024 chunk_size=64 verify=0" in result.stdout
    assert "dry_run: case=dataplane command=" in result.stdout
    assert "run_ub_eight_node_obmm_dataplane_microbench.sh" in result.stdout
    assert "DP_MODES_OVERRIDE=legacy-pa\\ generic-gva\\ gsva" in result.stdout
    assert "dry_run: case=tcp command=" in result.stdout
    assert "run_ub_eight_node_tcp_each_server_matrix.sh" in result.stdout
    assert "TCP_BENCHMARK=1" in result.stdout
    assert "PAIR_LIST_OVERRIDE=nodeA\\ nodeB" in result.stdout
    assert "dry_run: summary_command=" in result.stdout
    assert "transport_perf_report.py" in result.stdout
