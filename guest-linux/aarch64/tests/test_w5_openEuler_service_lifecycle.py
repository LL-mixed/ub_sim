#!/usr/bin/env python3
"""Contract tests for the W5 openEuler guest service lifecycle."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_RUNNER = ROOT / "guest-linux" / "aarch64" / "scripts" / (
    "run_llm_infer_eight_node_guest.sh"
)


class W5OpenEulerServiceLifecycleTest(unittest.TestCase):
    def test_openEuler_overlay_replaces_the_canonical_guest_service(self):
        source = GUEST_RUNNER.read_text(encoding="utf-8")

        self.assertIn(
            'cat > "$unit_dir/linqu-w5-guest.service"',
            source,
        )
        self.assertIn(
            '"$overlay_dir/etc/systemd/system/multi-user.target.wants/'
            'linqu-w5-guest.service"',
            source,
        )
        self.assertNotIn('cat > "$unit_dir/ub-w5.service"', source)
        self.assertNotIn(
            'multi-user.target.wants/ub-w5.service',
            source,
        )
        self.assertIn("After=network.target", source)
        self.assertNotIn("After=multi-user.target", source)


if __name__ == "__main__":
    unittest.main()
