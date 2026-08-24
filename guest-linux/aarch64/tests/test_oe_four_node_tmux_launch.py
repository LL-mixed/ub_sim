#!/usr/bin/env python3
"""Contract tests for the interactive openEuler tmux cluster launcher."""

import pathlib
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPTS = REPO / "guest-linux" / "aarch64" / "scripts"

LAUNCHER = SCRIPTS / "launch_oe_four_node_tmux.sh"
OE_SUPER = SCRIPTS / "run-openEuler-simulated-super-node.sh"


class OeFourNodeTmuxLaunchTest(unittest.TestCase):
    def test_launcher_boots_cluster_and_builds_console_panes(self):
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("run-openEuler-simulated-super-node.sh", source)
        self.assertIn("--nodes 4", source)
        self.assertIn("--run-id", source)
        # Panes run socat directly (no send-keys shell race).
        self.assertIn("socat - UNIX-CONNECT:", source)
        self.assertNotIn("send-keys", source)
        for node in ("nodeA", "nodeB", "nodeC", "nodeD"):
            self.assertIn(f"UNIX-CONNECT:$SERIAL_DIR/{node}.sock", source)
            self.assertIn(f'-T {node}', source)

    def test_launcher_waits_for_serial_sockets_and_prints_help(self):
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("-S \"$SERIAL_DIR/nodeD.sock\"", source)
        self.assertIn("synchronize-panes", source)
        self.assertIn("pane-border-status top", source)
        self.assertIn("select-layout", source)
        self.assertIn("CLEANUP_SCRIPT=", source)

    def test_launcher_rejects_existing_session_and_missing_disk(self):
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("tmux has-session", source)
        self.assertIn("disk image not found", source)


if __name__ == "__main__":
    unittest.main()
