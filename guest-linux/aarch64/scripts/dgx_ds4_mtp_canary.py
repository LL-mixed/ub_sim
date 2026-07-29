#!/usr/bin/env python3
"""Run a reversible DGX Spark ds4 single-node MTP canary."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse


SCRIPT_DIR = Path(__file__).resolve().parent
FETCH_SCRIPT = SCRIPT_DIR / "dgx_ds4_fetch.py"
DEFAULT_ENDPOINT = "http://192.168.8.7:8000"
DEFAULT_REMOTE_DIR = "/home/dgx/repo/ds4"
DEFAULT_Q2_MODEL = (
    "gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-"
    "chat-v2-imatrix.gguf"
)
DEFAULT_MTP_MODEL = "gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf"


class CanaryError(RuntimeError):
    """Raised when a canary stage cannot safely continue."""


@dataclass(frozen=True)
class RemoteService:
    pid: int
    cwd: str
    argv: list[str]
    stdout_path: str


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def non_negative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return parsed


def non_negative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run Q2 no-MTP and Q2+MTP canaries on dgx1, collect logs locally, "
            "and restore the original distributed coordinator."
        )
    )
    parser.add_argument("--prompt-file", type=Path, required=True)
    parser.add_argument("--ssh-host", default="dgx1")
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    parser.add_argument(
        "--benchmark-via-ssh",
        action="store_true",
        help="run the HTTP benchmark on --ssh-host via its loopback endpoint",
    )
    parser.add_argument("--remote-dir", default=DEFAULT_REMOTE_DIR)
    parser.add_argument("--q2-model", default=DEFAULT_Q2_MODEL)
    parser.add_argument("--mtp-model", default=DEFAULT_MTP_MODEL)
    parser.add_argument(
        "--mtp-mode",
        choices=(
            "instrumented-fast",
            "clean-fast",
            "clean-strict",
            "clean-exact-replay",
        ),
        default="instrumented-fast",
        help="MTP execution mode (default: instrumented-fast)",
    )
    parser.add_argument(
        "--mtp-margin",
        type=non_negative_float,
        default=3.0,
        help="MTP confidence margin (default: 3)",
    )
    parser.add_argument("--ctx", type=positive_int, default=4096)
    parser.add_argument("--max-tokens", type=positive_int, default=1024)
    parser.add_argument("--runs", type=positive_int, default=3)
    parser.add_argument("--warmup-runs", type=non_negative_int, default=1)
    parser.add_argument("--request-timeout", type=positive_int, default=600)
    parser.add_argument("--startup-timeout", type=positive_int, default=180)
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="local artifact directory (default: out/dgx-mtp-canary/<timestamp>)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the planned commands without changing the remote service",
    )
    return parser


def direct_http_ready(endpoint: str, timeout: float = 3.0) -> bool:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    request = urllib.request.Request(
        f"{endpoint.rstrip('/')}/v1/models", method="GET"
    )
    try:
        with opener.open(request, timeout=timeout) as response:
            return response.status == 200
    except (OSError, urllib.error.URLError):
        return False


def remote_path(remote_dir: str, path: str) -> str:
    if path.startswith("/"):
        return path
    return f"{remote_dir.rstrip('/')}/{path}"


def server_argv(args: argparse.Namespace, mtp: bool) -> list[str]:
    endpoint = urlparse(args.endpoint)
    if endpoint.scheme != "http" or not endpoint.hostname or not endpoint.port:
        raise CanaryError("endpoint must be an explicit http://host:port URL")
    argv = [
        "./ds4-server",
        "--cuda",
        "-m",
        args.q2_model,
        "--host",
        "0.0.0.0",
        "--port",
        str(endpoint.port),
        "--ctx",
        str(args.ctx),
        "--tokens",
        str(args.max_tokens),
    ]
    if mtp:
        argv.extend(
            [
                "--mtp",
                args.mtp_model,
                "--mtp-draft",
                "2",
                "--mtp-margin",
                f"{args.mtp_margin:g}",
            ]
        )
    return argv


def mtp_environment(args: argparse.Namespace) -> dict[str, str]:
    if args.mtp_mode == "instrumented-fast":
        return {
            "DS4_MTP_TIMING": "1",
            "DS4_MTP_SPEC_LOG": "1",
            "DS4_MTP_CONF_LOG": "1",
        }
    if args.mtp_mode == "clean-strict":
        return {"DS4_MTP_STRICT": "1"}
    if args.mtp_mode == "clean-exact-replay":
        return {"DS4_MTP_EXACT_REPLAY": "1"}
    return {}


def benchmark_argv(
    args: argparse.Namespace,
    output_path: Path | str,
    label: str,
    *,
    python: str | None = None,
    fetch_script: Path | str | None = None,
    endpoint: str | None = None,
    prompt_file: Path | str | None = None,
) -> list[str]:
    return [
        python or sys.executable,
        str(fetch_script or FETCH_SCRIPT),
        "--endpoint",
        endpoint or args.endpoint,
        "--timeout",
        str(args.request_timeout),
        "--output",
        str(output_path),
        "benchmark",
        "--label",
        label,
        "--prompt-file",
        str(prompt_file or args.prompt_file),
        "--runs",
        str(args.runs),
        "--warmup-runs",
        str(args.warmup_runs),
        "--max-tokens",
        str(args.max_tokens),
    ]


def build_comparison(
    baseline: dict[str, object],
    mtp: dict[str, object],
    mtp_log: str,
    *,
    require_timing_evidence: bool = True,
) -> dict[str, object]:
    baseline_tpot = float(baseline["summary"]["tpot_ms_median"])
    mtp_tpot = float(mtp["summary"]["tpot_ms_median"])
    baseline_outputs = [run["output_text"] for run in baseline["runs"]]
    mtp_outputs = [run["output_text"] for run in mtp["runs"]]
    timing_records = re.findall(
        r"mtp timing .*?drafted=(\d+).*?committed=(\d+)", mtp_log
    )
    drafted_multi = sum(int(drafted) > 1 for drafted, _ in timing_records)
    committed_multi = sum(int(committed) > 1 for _, committed in timing_records)
    outputs_match = baseline_outputs == mtp_outputs
    performance_improved = mtp_tpot < baseline_tpot
    exercised = drafted_multi > 0
    model_loaded = "MTP support model loaded" in mtp_log
    evidence_passed = exercised if require_timing_evidence else model_loaded
    return {
        "status": "pass" if outputs_match and evidence_passed else "fail",
        "correctness": {
            "outputs_match": outputs_match,
            "baseline_runs": len(baseline_outputs),
            "mtp_runs": len(mtp_outputs),
        },
        "speculation": {
            "timing_records": len(timing_records),
            "multi_token_drafts": drafted_multi,
            "multi_token_commits": committed_multi,
            "model_loaded": model_loaded,
            "timing_evidence_required": require_timing_evidence,
        },
        "performance": {
            "baseline_tpot_ms_median": baseline_tpot,
            "mtp_tpot_ms_median": mtp_tpot,
            "performance_improved": performance_improved,
            "speedup": round(baseline_tpot / mtp_tpot, 4),
            "reduction_percent": round(
                (baseline_tpot - mtp_tpot) * 100.0 / baseline_tpot, 3
            ),
        },
    }


class CanaryRunner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        self.run_id = f"ds4-mtp-canary-{stamp}"
        self.output_dir = args.output_dir or (
            Path("out") / "dgx-mtp-canary" / stamp
        )
        self.output_dir = self.output_dir.resolve()
        self.remote_env = f"/tmp/{self.run_id}.env"
        self.remote_baseline_log = f"/tmp/{self.run_id}-q2-no-mtp.log"
        self.remote_mtp_log = f"/tmp/{self.run_id}-q2-mtp.log"
        self.remote_fetch_script = f"/tmp/{self.run_id}-fetch.py"
        self.remote_prompt = f"/tmp/{self.run_id}-prompt.txt"
        self.active_pid: int | None = None
        self.original: RemoteService | None = None
        self.original_stopped = False
        self.event_log: Path | None = None

    def log_event(self, message: str) -> None:
        line = f"{datetime.now().isoformat(timespec='seconds')} {message}\n"
        print(f"dgx_mtp_canary: {message}", file=sys.stderr)
        if self.event_log is not None:
            with self.event_log.open("a", encoding="utf-8", newline="\n") as log:
                log.write(line)

    def remote(
        self,
        argv: list[str],
        *,
        input_text: str | None = None,
        check: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            self.args.ssh_host,
            shlex.join(argv),
        ]
        result = subprocess.run(
            command,
            input=input_text,
            text=True,
            capture_output=True,
            check=False,
        )
        if check and result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise CanaryError(f"remote command failed: {detail}")
        return result

    def ssh_argv(self, argv: list[str]) -> list[str]:
        return [
            "ssh",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            self.args.ssh_host,
            shlex.join(argv),
        ]

    def stream_command(self, argv: list[str]) -> None:
        self.log_event(f"command={shlex.join(argv)}")
        process = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="", file=sys.stderr)
            if self.event_log is not None:
                with self.event_log.open(
                    "a", encoding="utf-8", newline="\n"
                ) as log:
                    log.write(line)
        returncode = process.wait()
        if returncode != 0:
            raise CanaryError(f"command failed with exit code {returncode}")

    def local(self, argv: list[str]) -> None:
        self.stream_command(argv)

    def copy_to_remote(self, local_path: Path, remote_path_value: str) -> None:
        command = [
            "scp",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            str(local_path),
            f"{self.args.ssh_host}:{remote_path_value}",
        ]
        result = subprocess.run(command, check=False)
        if result.returncode != 0:
            raise CanaryError(f"failed to copy {local_path} to dgx1")

    def benchmark_endpoint(self) -> str:
        if not self.args.benchmark_via_ssh:
            return self.args.endpoint
        endpoint = urlparse(self.args.endpoint)
        if not endpoint.port:
            raise CanaryError("endpoint must include a port")
        return f"http://127.0.0.1:{endpoint.port}"

    def http_ready(self) -> bool:
        if not self.args.benchmark_via_ssh:
            return direct_http_ready(self.args.endpoint)
        result = self.remote(
            [
                "curl",
                "--noproxy",
                "*",
                "--connect-timeout",
                "3",
                "--max-time",
                "5",
                "-fsS",
                f"{self.benchmark_endpoint()}/v1/models",
            ],
            check=False,
        )
        return result.returncode == 0

    def capture_original(self) -> RemoteService:
        result = self.remote(["pgrep", "-x", "ds4-server"], check=False)
        pids = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        if result.returncode != 0 or len(pids) != 1 or not pids[0].isdigit():
            raise CanaryError(
                "expected exactly one running ds4-server coordinator on dgx1"
            )
        code = (
            "import json,os,pathlib,sys;"
            "p=sys.argv[1];"
            "a=[x.decode() for x in pathlib.Path('/proc/'+p+'/cmdline')."
            "read_bytes().split(b'\\0') if x];"
            "print(json.dumps({'pid':int(p),'cwd':os.readlink('/proc/'+p+'/cwd'),"
            "'argv':a,'stdout_path':os.readlink('/proc/'+p+'/fd/1')}))"
        )
        metadata = json.loads(
            self.remote(["python3", "-c", code, pids[0]]).stdout
        )
        stdout_path = metadata["stdout_path"]
        if not stdout_path.startswith("/"):
            raise CanaryError("original coordinator stdout is not a restorable file")
        return RemoteService(**metadata)

    def preflight(self) -> None:
        if not self.args.prompt_file.is_file():
            raise CanaryError(f"prompt file not found: {self.args.prompt_file}")
        self.remote(["true"])
        for path in (self.args.q2_model, self.args.mtp_model):
            self.remote(["test", "-f", remote_path(self.args.remote_dir, path)])
        self.original = self.capture_original()
        if "--role" not in self.original.argv or "coordinator" not in self.original.argv:
            raise CanaryError(
                "the running ds4-server is not the expected distributed coordinator"
            )
        for required_option in ("--layers", "--listen"):
            if required_option not in self.original.argv:
                raise CanaryError(
                    "the running distributed coordinator is missing "
                    f"{required_option}"
                )
        if Path(self.original.cwd) != Path(self.args.remote_dir):
            raise CanaryError(
                "the coordinator cwd differs from --remote-dir: "
                f"{self.original.cwd} != {self.args.remote_dir}"
            )
        if not self.http_ready():
            raise CanaryError(
                "the current coordinator is not reachable at "
                f"{self.args.endpoint}; no process was stopped"
            )

    def write_remote_env(self) -> None:
        content = "".join(
            f"{name}={value}\n"
            for name, value in mtp_environment(self.args).items()
        )
        self.remote(["tee", self.remote_env], input_text=content)
        self.remote(["chmod", "600", self.remote_env])

    def process_alive(self, pid: int) -> bool:
        return self.remote(["kill", "-0", str(pid)], check=False).returncode == 0

    def stop_process(self, pid: int) -> None:
        if not self.process_alive(pid):
            return
        self.remote(["kill", str(pid)])
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if not self.process_alive(pid):
                return
            time.sleep(0.5)
        raise CanaryError(f"remote process {pid} did not stop after SIGTERM")

    def start_service(
        self,
        argv: list[str],
        cwd: str,
        log_path: str,
        *,
        env_path: str | None = None,
        append_log: bool = False,
    ) -> int:
        lines = [f"cd {shlex.quote(cwd)} || exit 1"]
        if env_path is not None:
            lines.extend(
                ["set -a", f". {shlex.quote(env_path)}", "set +a"]
            )
        redirect = ">>" if append_log else ">"
        lines.extend(
            [
                (
                    f"nohup {shlex.join(argv)} {redirect} {shlex.quote(log_path)} "
                    "2>&1 < /dev/null &"
                ),
                "pid=$!",
                "printf '%s\\n' \"$pid\"",
            ]
        )
        result = self.remote(["sh", "-lc", "\n".join(lines)])
        pid_text = result.stdout.strip().splitlines()[-1]
        if not pid_text.isdigit():
            raise CanaryError(f"failed to read remote service PID: {result.stdout!r}")
        pid = int(pid_text)
        self.active_pid = pid
        time.sleep(0.5)
        if not self.process_alive(pid):
            raise CanaryError(f"remote service exited during startup; log={log_path}")
        return pid

    def wait_http(self, expected_up: bool) -> None:
        deadline = time.monotonic() + self.args.startup_timeout
        while time.monotonic() < deadline:
            if self.http_ready() == expected_up:
                return
            if expected_up and self.active_pid and not self.process_alive(self.active_pid):
                raise CanaryError("canary service exited before HTTP became ready")
            time.sleep(1)
        state = "ready" if expected_up else "stopped"
        raise CanaryError(f"HTTP endpoint did not become {state}")

    def run_benchmark(self, label: str, output_path: Path) -> None:
        if not self.args.benchmark_via_ssh:
            self.local(benchmark_argv(self.args, output_path, label))
            return
        remote_output = f"/tmp/{self.run_id}-{label}.json"
        command = benchmark_argv(
            self.args,
            remote_output,
            label,
            python="python3",
            fetch_script=self.remote_fetch_script,
            endpoint=self.benchmark_endpoint(),
            prompt_file=self.remote_prompt,
        )
        self.stream_command(self.ssh_argv(command))
        self.copy_remote_log(remote_output, output_path)

    def copy_remote_log(self, remote_path_value: str, local_path: Path) -> None:
        exists = self.remote(["test", "-f", remote_path_value], check=False)
        if exists.returncode != 0:
            return
        result = subprocess.run(
            [
                "scp",
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=5",
                f"{self.args.ssh_host}:{remote_path_value}",
                str(local_path),
            ],
            check=False,
        )
        if result.returncode != 0:
            raise CanaryError(f"failed to copy remote log {remote_path_value}")

    def stop_active(self) -> None:
        if self.active_pid is None:
            return
        self.stop_process(self.active_pid)
        self.active_pid = None
        self.wait_http(False)

    def restore_original(self) -> None:
        if not self.original_stopped or self.original is None:
            return
        existing = self.remote(["pgrep", "-x", "ds4-server"], check=False)
        if existing.returncode == 0 and existing.stdout.strip():
            raise CanaryError("refusing to restore over an existing ds4-server")
        self.active_pid = self.start_service(
            self.original.argv,
            self.original.cwd,
            self.original.stdout_path,
            append_log=True,
        )
        self.wait_http(True)
        self.active_pid = None
        self.original_stopped = False

    def dry_run(self) -> None:
        plan = {
            "ssh_host": self.args.ssh_host,
            "endpoint": self.args.endpoint,
            "benchmark_via_ssh": self.args.benchmark_via_ssh,
            "benchmark_endpoint": self.benchmark_endpoint(),
            "remote_env": self.remote_env,
            "mtp_mode": self.args.mtp_mode,
            "mtp_environment": mtp_environment(self.args),
            "baseline_server": server_argv(self.args, mtp=False),
            "mtp_server": server_argv(self.args, mtp=True),
            "output_dir": str(self.output_dir),
        }
        print(json.dumps(plan, ensure_ascii=False, indent=2))

    def run(self) -> Path:
        if self.args.dry_run:
            self.dry_run()
            return self.output_dir

        self.output_dir.mkdir(parents=True, exist_ok=False)
        baseline_json = self.output_dir / "q2-no-mtp.json"
        mtp_json = self.output_dir / "q2-mtp.json"
        baseline_log = self.output_dir / "q2-no-mtp.log"
        mtp_log = self.output_dir / "q2-mtp.log"
        state_path = self.output_dir / "canary-state.json"
        self.event_log = self.output_dir / "canary.log"
        state: dict[str, object] = {"run_id": self.run_id, "status": "running"}
        state_path.write_text(
            json.dumps(state, indent=2) + "\n", encoding="utf-8", newline="\n"
        )

        primary_error: Exception | None = None
        try:
            self.log_event("stage=preflight")
            self.preflight()
            self.write_remote_env()
            if self.args.benchmark_via_ssh:
                self.copy_to_remote(FETCH_SCRIPT, self.remote_fetch_script)
                self.copy_to_remote(self.args.prompt_file, self.remote_prompt)
            assert self.original is not None
            state["original_service"] = {
                "pid": self.original.pid,
                "cwd": self.original.cwd,
                "argv": self.original.argv,
                "stdout_path": self.original.stdout_path,
            }

            self.log_event("stage=stop-distributed-coordinator")
            self.stop_process(self.original.pid)
            self.original_stopped = True
            self.wait_http(False)

            self.log_event("stage=q2-no-mtp")
            self.active_pid = self.start_service(
                server_argv(self.args, mtp=False),
                self.args.remote_dir,
                self.remote_baseline_log,
            )
            self.wait_http(True)
            self.run_benchmark("q2-no-mtp", baseline_json)
            self.stop_active()
            self.copy_remote_log(self.remote_baseline_log, baseline_log)

            self.log_event("stage=q2-mtp")
            self.active_pid = self.start_service(
                server_argv(self.args, mtp=True),
                self.args.remote_dir,
                self.remote_mtp_log,
                env_path=self.remote_env,
            )
            self.wait_http(True)
            self.run_benchmark(f"q2-mtp-{self.args.mtp_mode}", mtp_json)
            self.stop_active()
            self.copy_remote_log(self.remote_mtp_log, mtp_log)

            comparison = build_comparison(
                json.loads(baseline_json.read_text(encoding="utf-8")),
                json.loads(mtp_json.read_text(encoding="utf-8")),
                mtp_log.read_text(encoding="utf-8"),
                require_timing_evidence=(
                    self.args.mtp_mode == "instrumented-fast"
                ),
            )
            (self.output_dir / "comparison.json").write_text(
                json.dumps(comparison, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            state["comparison"] = comparison
            state["status"] = comparison["status"]
        except Exception as error:
            primary_error = error
            state["status"] = "failed"
            state["error"] = str(error)
        finally:
            cleanup_errors: list[str] = []
            try:
                if self.active_pid is not None:
                    self.stop_process(self.active_pid)
                    self.active_pid = None
            except Exception as cleanup_error:
                cleanup_errors.append(f"stop canary service: {cleanup_error}")

            for remote_log, local_log in (
                (self.remote_baseline_log, baseline_log),
                (self.remote_mtp_log, mtp_log),
            ):
                try:
                    self.copy_remote_log(remote_log, local_log)
                except Exception as cleanup_error:
                    cleanup_errors.append(f"copy {remote_log}: {cleanup_error}")

            self.log_event("stage=restore-coordinator")
            try:
                self.restore_original()
            except Exception as cleanup_error:
                cleanup_errors.append(
                    f"restore original coordinator: {cleanup_error}"
                )

            state["coordinator_restored"] = not self.original_stopped
            if cleanup_errors:
                state["cleanup_errors"] = cleanup_errors
                for cleanup_error in cleanup_errors:
                    self.log_event(f"cleanup-error={cleanup_error}")
                if primary_error is None:
                    primary_error = CanaryError(cleanup_errors[0])

            if primary_error is None and state.get("status") != "pass":
                primary_error = CanaryError(
                    "canary comparison failed; see "
                    f"{self.output_dir / 'comparison.json'}"
                )
                state["error"] = str(primary_error)

            state_path.write_text(
                json.dumps(state, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )

        if primary_error is not None:
            raise CanaryError(str(primary_error)) from primary_error
        return self.output_dir


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        output_dir = CanaryRunner(args).run()
    except (CanaryError, OSError, json.JSONDecodeError) as error:
        print(f"dgx_mtp_canary: status=failed reason={error}", file=sys.stderr)
        return 2
    if not args.dry_run:
        print(f"dgx_mtp_canary: status=complete output_dir={output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
