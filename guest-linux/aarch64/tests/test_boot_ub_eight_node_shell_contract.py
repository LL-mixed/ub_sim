#!/usr/bin/env python3

import subprocess
import unittest
from pathlib import Path


class BootUbEightNodeShellContractTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path(__file__).resolve().parents[3]
        self.script = (
            self.repo
            / "guest-linux"
            / "aarch64"
            / "scripts"
            / "boot_ub_eight_node_shell.sh"
        )

    def test_script_exists_and_is_executable(self):
        self.assertTrue(self.script.is_file())
        self.assertTrue(self.script.stat().st_mode & 0o111)

    def test_zsh_syntax_ok(self):
        result = subprocess.run(
            ["zsh", "-n", str(self.script)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_help_lists_subcommands(self):
        result = subprocess.run(
            [str(self.script), "--help"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for token in ("up", "attach", "status", "stop", "nodeA"):
            self.assertIn(token, result.stderr)

    def test_unknown_subcommand_rejected(self):
        result = subprocess.run(
            [str(self.script), "bogus"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 2)

    def test_guest_runner_template_is_interactive_shell(self):
        source = self.script.read_text()
        # The guest-side runner must hand the serial console to an interactive
        # root bash (not the W5 workload), reading AND writing ttyAMA0.
        self.assertIn("setsid -c /bin/bash -l </dev/ttyAMA0 >/dev/ttyAMA0", source)
        self.assertIn("chmod +x \"$ENV_DIR/runner.sh\"", source)

    def test_delegates_to_eight_node_launcher_with_openeuler_backend(self):
        source = self.script.read_text()
        self.assertIn("launch_ub_eight_node_headless.sh", source)
        # QEMU runs in a long-lived container (prebuilt binary needs container
        # libs); sockets are bind-mounted to the host for attach.
        self.assertIn("docker run -d", source)
        self.assertIn("UB_USE_PREBUILT_QEMU=1", source)
        self.assertIn("SIM_W5_GUEST_BACKEND=openeuler", source)


if __name__ == "__main__":
    unittest.main()
