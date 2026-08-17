#!/usr/bin/env python3

import subprocess
import unittest
from pathlib import Path


class OpenEulerGuestBackendTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path(__file__).resolve().parents[3]
        self.script_dir = self.repo / "guest-linux" / "aarch64" / "scripts"
        self.builder = self.script_dir / "build_openeuler_guest.sh"
        self.launcher = self.script_dir / "launch_ub_eight_node_headless.sh"
        self.guest_runner = self.script_dir / "run_llm_infer_eight_node_guest.sh"
        self.container = self.script_dir / "run_w5_in_container.sh"

    def test_builder_cli_help(self):
        result = subprocess.run(
            [str(self.builder), "--help"],
            cwd=self.repo,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("build_openeuler_guest.sh", result.stderr)
        self.assertIn("--rootfs-source", result.stderr)

    def test_builder_cli_rejects_unknown_rootfs_source(self):
        result = subprocess.run(
            [str(self.builder), "--rootfs-source", "bogus"],
            cwd=self.repo,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown --rootfs-source: bogus", result.stderr)

    def test_builder_masks_all_gettys(self):
        source = self.builder.read_text()
        for unit in (
            "serial-getty@ttyAMA0.service",
            "console-getty.service",
            "getty@tty1.service",
        ):
            self.assertIn(
                f"ln -sf /dev/null /mnt/etc/systemd/system/{unit}", source
            )
            self.assertIn(
                f"rm -f /mnt/etc/systemd/system/getty.target.wants/{unit}", source
            )

    def test_builder_does_not_install_autologin_getty(self):
        source = self.builder.read_text()
        self.assertNotIn("autologin.conf", source)
        self.assertNotIn("--autologin", source)

    def test_builder_keeps_busybox_pipeline_untouched(self):
        source = self.builder.read_text()
        self.assertIn("Does NOT touch the existing cpio+rdinit pipeline", source)

    def test_launcher_validates_guest_backend_values(self):
        source = self.launcher.read_text()
        self.assertIn('SIM_W5_GUEST_BACKEND="${SIM_W5_GUEST_BACKEND:-busybox}"', source)
        self.assertIn("busybox|openeuler) ;;", source)
        self.assertIn("unknown SIM_W5_GUEST_BACKEND=", source)

    def test_launcher_wires_openeuler_qemu_args(self):
        source = self.launcher.read_text()
        self.assertIn('mount_tag=w5env', source)
        self.assertIn('mount_tag=w5tools', source)
        self.assertIn('mount_tag=w5modules', source)
        self.assertIn("root=/dev/vda rw rootfstype=ext4 init=/init", source)

    def test_guest_runner_writes_openeuler_env_and_runner(self):
        source = self.guest_runner.read_text()
        self.assertIn("write_w4_openeuler_env()", source)
        self.assertIn("configure_ipourma_network", source)
        self.assertIn('w4guest8:(initramfs|openeuler)', source)

    def test_guest_runner_has_no_ipourma_dump_instrumentation(self):
        source = self.guest_runner.read_text()
        self.assertNotIn("dump_ipourma_stats", source)
        self.assertNotIn("periodic_dumps_", source)

    def test_container_forwards_guest_backend_env(self):
        source = self.container.read_text()
        self.assertIn('-e "SIM_W5_GUEST_BACKEND=$SIM_W5_GUEST_BACKEND"', source)
        self.assertIn('-e "SIM_W5_OPENEULER_ROOTFS=$SIM_W5_OPENEULER_ROOTFS"', source)


if __name__ == "__main__":
    unittest.main()
