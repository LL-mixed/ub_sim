#!/usr/bin/env python3
"""Contract tests for the obmm-test guest integration."""

import pathlib
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
AARCH64 = REPO / "guest-linux" / "aarch64"

APP = AARCH64 / "apps" / "obmm_test" / "obmm_test.c"
APP_MAKEFILE = AARCH64 / "apps" / "obmm_test" / "Makefile"
RUN_APP = AARCH64 / "initramfs" / "run_app"
BUILD_INITRAMFS = AARCH64 / "scripts" / "build_initramfs.sh"
RUN_SCRIPT = AARCH64 / "scripts" / "run_ub_dual_node_obmm_test.sh"


class ObmmTestGuestWiringTest(unittest.TestCase):
    def test_app_stages_conductor_and_orchestrator(self):
        source = APP.read_text(encoding="utf-8")
        self.assertIn("/bin/obmm-test-conductor", source)
        self.assertIn("/bin/obmm-test-orchestrator", source)
        # Non-invasive readiness: no bare connect() probe that would kill the
        # conductor's accept() loop.
        self.assertNotIn("probe_tcp(", source)
        self.assertIn("/proc/net/tcp", source)
        self.assertIn("bring_up_loopback", source)

    def test_run_app_dispatches_obmm_test_flag(self):
        source = RUN_APP.read_text(encoding="utf-8")
        self.assertIn("run_obmm_test()", source)
        self.assertIn('has_flag "linqu_obmm_test=1"', source)
        self.assertIn('run_binary "linqu_obmm_test" /bin/linqu_obmm_test', source)
        for var in ("OBMM_TEST_PORT", "OBMM_TEST_GROUP_INCLUDE",
                    "OBMM_TEST_GROUP_EXCLUDE", "OBMM_TEST_INCLUDE",
                    "OBMM_TEST_EXCLUDE", "OBMM_TEST_TIMEOUT_SCALE"):
            self.assertIn(var, source)

    def test_build_initramfs_compiles_and_installs_app(self):
        source = BUILD_INITRAMFS.read_text(encoding="utf-8")
        self.assertIn('apps/obmm_test/obmm_test.c', source)
        self.assertIn('cp "$OBMM_TEST_APP_BIN" "$INITRAMFS_DIR/bin/linqu_obmm_test"', source)
        self.assertIn('write_signature_line "obmm_test_app_src"', source)

    def test_run_script_stages_binaries_and_passes_filters(self):
        source = RUN_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("OBMM_TEST_BUILD_DIR", source)
        self.assertIn("cp \"$CONDUCTOR_BIN\"", source)
        self.assertIn("cp \"$ORCHESTRATOR_BIN\"", source)
        for var in ("OBMM_TEST_GROUP_INCLUDE", "OBMM_TEST_GROUP_EXCLUDE",
                    "OBMM_TEST_INCLUDE", "OBMM_TEST_EXCLUDE",
                    "OBMM_TEST_TIMEOUT_SCALE"):
            self.assertIn(f"${{{var}}}", source)
            cmdline_key = var.replace("OBMM_TEST_", "obmm_test_").lower()
            self.assertIn(f"{cmdline_key}=${{{var}}}", source)
        self.assertIn("kill_stale_obmm_test_vms", source)

    def test_run_script_rejects_missing_build_dir(self):
        result = subprocess.run(
            ["zsh", str(RUN_SCRIPT)],
            env={
                "PATH": "/usr/bin:/bin",
                "OBMM_TEST_BUILD_DIR": "/nonexistent-obmm-test-build",
            },
            capture_output=True,
            text=True,
            check=False,
            cwd=str(AARCH64),
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("missing", result.stderr)
        self.assertNotIn("build_qemu_binary", result.stderr)
        source = RUN_SCRIPT.read_text(encoding="utf-8")
        self.assertLess(source.index('for required in '),
                        source.index('QEMU_BIN="$(ensure_qemu_ub_binary'))


if __name__ == "__main__":
    unittest.main()
