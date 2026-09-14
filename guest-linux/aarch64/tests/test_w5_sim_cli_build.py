"""Exercise W5 executable discovery without invoking Cargo or loading models."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(__file__).resolve().parents[1] / "scripts" / (
    "run_w5_inference_cluster_runtime.sh"
)


class W5SimCliBuildTest(unittest.TestCase):
    def run_discovery(self, messages=None, cargo_status=0, explicit=False,
                      executable=True):
        source = RUNNER.read_text()
        start = source.index('  if [[ -z "${SIM_CLI_BIN:-}" ]]; then')
        end = source.index('  case "$SIM_UAPI_W5_PROFILE" in', start)
        # Execute the production build/validation block, stopping before inference.
        script = 'set -euo pipefail\nexport PATH="$TEST_CARGO_BIN:$PATH"\n'
        script += source[start:end]
        script += '\nprintf "%s\\n" "$SIM_CLI_BIN"\n'
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "custom target" / "target-triple" / "debug" / "sim-cli"
            binary.parent.mkdir(parents=True)
            binary.write_text("#!/bin/sh\nexit 0\n")
            binary.chmod(0o755 if executable else 0o644)
            artifact = {
                "reason": "compiler-artifact",
                "target": {"name": "sim-cli", "kind": ["bin"]},
                "executable": str(binary),
            }
            records = messages(artifact) if messages else [artifact]
            cargo = root / "cargo"
            cargo.write_text(
                f"#!{sys.executable}\n"
                "import os, pathlib, sys\n"
                "assert sys.argv[1:] == ['build', '-p', 'sim-cli', "
                "'--message-format=json-render-diagnostics']\n"
                "assert os.getcwd() == os.environ['REPO_DIR']\n"
                "pathlib.Path('cargo-called').touch()\n"
                f"print({chr(10).join(json.dumps(m) for m in records)!r})\n"
                f"sys.exit({cargo_status})\n"
            )
            cargo.chmod(0o755)
            environment = os.environ.copy()
            environment.pop("SIM_CLI_BIN", None)
            environment.update(REPO_DIR=str(root), TEST_CARGO_BIN=str(root))
            if explicit:
                environment["SIM_CLI_BIN"] = str(binary)
            for shell in ("bash", "zsh"):
                shell_path = shutil.which(shell)
                if shell_path is None:
                    continue
                with self.subTest(shell=shell):
                    marker = root / "cargo-called"
                    marker.unlink(missing_ok=True)
                    result = subprocess.run(
                        [shell_path, "-c", script], env=environment,
                        text=True, capture_output=True, timeout=10,
                    )
                    self.check_result(result, str(binary))
                    self.assertEqual(marker.exists(), not explicit)

    def check_result(self, result, binary):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), binary)

    def test_uses_cargo_artifact_with_spaces_and_target_triple(self):
        self.run_discovery()

    def test_ignores_other_artifacts_and_accepts_duplicate_path(self):
        self.run_discovery(messages=lambda a: [
            {"reason": "build-script-executed"},
            {**a, "target": {"name": "other", "kind": ["bin"]}},
            {**a, "target": {"name": "sim-cli", "kind": ["lib"]}},
            a, a,
        ])

    def test_explicit_binary_does_not_build(self):
        self.run_discovery(explicit=True)

    def test_build_failure_rejects_even_reported_executable(self):
        self.check_result = lambda r, _: self.assertNotEqual(r.returncode, 0)
        self.run_discovery(cargo_status=101)

    def test_missing_artifact_is_rejected(self):
        self.check_result = lambda r, _: self.assertNotEqual(r.returncode, 0)
        self.run_discovery(messages=lambda _: [{"reason": "build-finished"}])

    def test_ambiguous_artifacts_are_rejected(self):
        self.check_result = lambda r, _: self.assertNotEqual(r.returncode, 0)
        self.run_discovery(messages=lambda a: [a, {**a, "executable": "/other"}])

    def test_nonexecutable_artifact_is_rejected(self):
        self.check_result = lambda r, _: self.assertNotEqual(r.returncode, 0)
        self.run_discovery(executable=False)


if __name__ == "__main__":
    unittest.main()
