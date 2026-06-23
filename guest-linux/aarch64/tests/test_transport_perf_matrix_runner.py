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
    assert "transport_perf_matrix: profile=quick" in result.stdout
    assert "transport_perf_matrix: size=2097152 dp_iterations=1024 tcp_iterations=64 chunk_size=64 verify=0" in result.stdout
    assert "transport_perf_matrix: tcp_pair_wait_secs=120" in result.stdout
    assert "transport_perf_matrix: tcp_one_way=1" in result.stdout
    assert "transport_perf_matrix: tcp_progress_interval=64" in result.stdout
    assert "dry_run: case=dataplane entrypoint=" in result.stdout
    assert "profile=quick size=2097152 iterations=1024 chunk_size=64" in result.stdout
    assert "run_ub_eight_node_obmm_dataplane_microbench.sh" in result.stdout
    assert "dry_run: case=tcp entrypoint=" in result.stdout
    assert "profile=quick size=2097152 iterations=64 chunk_size=64" in result.stdout
    assert "run_ub_eight_node_tcp_each_server_matrix.sh" in result.stdout
    assert " command= env " not in result.stdout
    assert "dry_run: summary_command=" in result.stdout
    assert "transport_perf_report.py" in result.stdout


def test_transport_perf_matrix_runner_supports_reusable_tcp_smoke_profile():
    result = subprocess.run(
        [
            str(SCRIPT),
            "--dry-run",
            "--profile",
            "tcp-smoke",
            "--run-id",
            "unit_tcp_smoke",
            "--out-dir",
            "/tmp/unit_tcp_smoke",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert "transport_perf_matrix: profile=tcp-smoke" in result.stdout
    assert "transport_perf_matrix: dataplane=0 tcp=1" in result.stdout
    assert "transport_perf_matrix: size=2097152 dp_iterations=0 tcp_iterations=64 chunk_size=64 verify=0" in result.stdout
    assert "dry_run: case=dataplane entrypoint=" not in result.stdout
    assert "dry_run: case=tcp entrypoint=" in result.stdout
    assert " command= env " not in result.stdout


def test_transport_perf_matrix_runner_can_print_verbose_internal_wiring():
    result = subprocess.run(
        [
            str(SCRIPT),
            "--dry-run-verbose",
            "--profile",
            "tcp-smoke",
            "--run-id",
            "unit_tcp_verbose",
            "--out-dir",
            "/tmp/unit_tcp_verbose",
        ],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )

    assert "dry_run: case=tcp command= env" in result.stdout
    assert "TCP_BENCH_ONE_WAY=1" in result.stdout
    assert "PAIR_LIST_OVERRIDE=nodeA\\ nodeB" in result.stdout
