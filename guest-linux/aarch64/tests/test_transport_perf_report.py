import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "transport_perf_report.py"


def write_report(path, run_id, run_dir, extra_lines=()):
    lines = [
        f"run_id={run_id}",
        "result=PASS",
        f"run_dir={run_dir}",
    ]
    lines.extend(extra_lines)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_transport_perf_report_summarizes_dataplane_and_tcp_logs():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        dp_run = root / "dp_headless8"
        tcp_run = root / "tcp_headless8"
        dp_run.mkdir()
        tcp_run.mkdir()
        write_report(root / "dp.txt", "dp", dp_run, ("modes=legacy-pa,generic-gva,gsva",))
        write_report(root / "tcp.txt", "tcp", tcp_run, ("tcp_benchmark=1",))
        (dp_run / "nodeA_guest.log").write_text(
            "\n".join(
                [
                    "[obmm_dataplane_microbench] result=done mode=legacy-pa size=2097152 iterations=8192 chunk_size=64 reads=8192 writes=8192 read_bytes=524288 write_bytes=524288 verify_failures=0 duration_ms=20 read_mbps=25.000 write_mbps=25.000",
                    "[obmm_dataplane_microbench] result=done mode=generic-gva size=2097152 iterations=8192 chunk_size=64 reads=8192 writes=8192 read_bytes=524288 write_bytes=524288 verify_failures=0 duration_ms=10 read_mbps=50.000 write_mbps=50.000",
                    "[obmm_dataplane_microbench] result=done mode=gsva size=2097152 iterations=8192 chunk_size=64 reads=8192 writes=8192 read_bytes=524288 write_bytes=524288 verify_failures=0 duration_ms=8 read_mbps=62.500 write_mbps=62.500",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        (tcp_run / "nodeA_guest.log").write_text(
            "[ub_tcp_each_server] benchmark_result=done role=nodeA size=2097152 iterations=8192 chunk_size=64 reads=8192 writes=8192 read_bytes=524288 write_bytes=524288 verify_failures=0 duration_ms=40 read_mbps=12.500 write_mbps=12.500\n",
            encoding="utf-8",
        )

        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(root / "dp.txt"), str(root / "tcp.txt")],
            check=True,
            text=True,
            capture_output=True,
        )

    assert "transport_case: name=legacy-pa samples=1 duration_ms_median=20.000" in result.stdout
    assert "transport_case: name=generic-gva samples=1 duration_ms_median=10.000" in result.stdout
    assert "transport_case: name=gsva samples=1 duration_ms_median=8.000" in result.stdout
    assert "transport_case: name=tcp samples=1 duration_ms_median=40.000" in result.stdout
    assert "transport_delta: case=generic-gva baseline=legacy-pa duration_speedup=2.000" in result.stdout
    assert "transport_delta: case=gsva baseline=legacy-pa duration_speedup=2.500" in result.stdout


def test_transport_perf_report_json_output_contains_cases():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        run_dir = root / "run_headless8"
        run_dir.mkdir()
        write_report(root / "dp.txt", "dp", run_dir)
        (run_dir / "nodeA_guest.log").write_text(
            "[obmm_dataplane_microbench] result=done mode=legacy-pa size=4096 iterations=2 chunk_size=64 reads=2 writes=2 read_bytes=128 write_bytes=128 verify_failures=0 duration_ms=1 read_mbps=0.125 write_mbps=0.125\n",
            encoding="utf-8",
        )

        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--json", str(root / "dp.txt")],
            check=True,
            text=True,
            capture_output=True,
        )

    parsed = json.loads(result.stdout)
    assert parsed["cases"]["legacy-pa"]["samples"] == 1
    assert parsed["runs"][0]["run_id"] == "dp"
