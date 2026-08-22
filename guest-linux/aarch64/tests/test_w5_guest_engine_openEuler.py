#!/usr/bin/env python3
"""Contract tests for the openEuler W5 guest engine."""

import pathlib
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
AARCH64 = REPO / "guest-linux" / "aarch64"
SCRIPTS = AARCH64 / "scripts"

GUEST_RUNNER = SCRIPTS / "run_llm_infer_eight_node_guest.sh"
LAUNCHER = SCRIPTS / "launch_ub_eight_node_headless.sh"
OE_WRAPPER = SCRIPTS / "run_w5_cluster_qwen3_0_6b_2step_openEuler.sh"
SWITCH_ROOT = AARCH64 / "initramfs" / "init_switch_root"
COMMON = SCRIPTS / "qemu_ub_common.sh"
OE_SUPER = SCRIPTS / "run-openEuler-simulated-super-node.sh"


class W5GuestEngineOpenEulerTest(unittest.TestCase):
    def test_guest_runner_validates_engine_and_disk(self):
        source = GUEST_RUNNER.read_text(encoding="utf-8")
        self.assertIn('SIM_W5_GUEST_ENGINE="${SIM_W5_GUEST_ENGINE:-busybox}"', source)
        self.assertIn("SIM_W5_GUEST_ENGINE must be busybox or openEuler", source)
        self.assertIn("openEuler guest engine requires SIM_W5_OE_DISK_IMAGE", source)
        self.assertIn("build_w4_openEuler_initramfs", source)

    def test_guest_runner_builds_root_overlay_with_unit(self):
        source = GUEST_RUNNER.read_text(encoding="utf-8")
        self.assertIn("ub_root_overlay", source)
        self.assertIn("/bin/linqu_*", source)
        self.assertIn("write_w5_openEuler_systemd_unit", source)
        self.assertIn("ExecStart=/bin/busybox sh /bin/run_app", source)
        self.assertIn("WantedBy=multi-user.target", source)

    def test_switch_root_deploys_overlay_resolving_usrmerge(self):
        source = SWITCH_ROOT.read_text(encoding="utf-8")
        self.assertIn("/ub_root_overlay", source)
        # busybox cp -a silently skips dirs whose dest is a symlink to a dir
        # (usrmerge /bin -> usr/bin); deploy must resolve destinations first.
        self.assertIn("readlink", source)

    def test_launcher_boots_disk_with_permissive_selinux(self):
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("SIM_W5_OE_DISK_IMAGE", source)
        self.assertIn("oe_overlays", source)
        self.assertIn("qemu-img create -f qcow2 -b", source)
        self.assertIn("init=/init enforcing=0", source)

    def test_common_shares_oe_helpers(self):
        source = COMMON.read_text(encoding="utf-8")
        self.assertIn("oe_ensure_lvm2_staging()", source)
        self.assertIn("oe_build_boot_skeleton()", source)

    def test_oe_wrapper_selects_engine(self):
        source = OE_WRAPPER.read_text(encoding="utf-8")
        self.assertIn("SIM_W5_GUEST_ENGINE=openEuler", source)
        self.assertIn("run_w5_cluster_qwen3_0_6b_2step.sh", source)

    def test_oe_super_node_script_defaults_busybox(self):
        source = OE_SUPER.read_text(encoding="utf-8")
        self.assertIn('BUSYBOX="${BUSYBOX:-$ROOT_DIR/busybox-aarch64}"', source)

    def test_engine_rejects_unknown_value(self):
        result = subprocess.run(
            [
                "zsh",
                str(GUEST_RUNNER),
            ],
            env={
                "PATH": "/usr/bin:/bin",
                "SIM_W5_CLUSTER_NODE_COUNT": "4",
                "SIM_W5_GUEST_ENGINE": "kvm",
            },
            capture_output=True,
            text=True,
            check=False,
            cwd=str(AARCH64),
        )
        self.assertEqual(2, result.returncode)
        self.assertIn("SIM_W5_GUEST_ENGINE must be busybox or openEuler: kvm", result.stderr)


if __name__ == "__main__":
    unittest.main()
